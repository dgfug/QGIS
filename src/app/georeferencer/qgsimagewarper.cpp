/***************************************************************************
     qgsimagewarper.cpp
     --------------------------------------
    Date                 : Sun Sep 16 12:03:14 AKDT 2007
    Copyright            : (C) 2007 by Gary E. Sherman
    Email                : sherman at mrcc dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <cmath>
#include <cstdio>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>

#include <QFile>
#include <QProgressDialog>

#include "qgsimagewarper.h"
#include "qgsgeoreftransform.h"
#include "qgslogger.h"
#include "qgsogrutils.h"

bool QgsImageWarper::sWarpCanceled = false;

QgsImageWarper::QgsImageWarper( QWidget *parent )
  : mParent( parent )
{
}

bool QgsImageWarper::openSrcDSAndGetWarpOpt( const QString &input, ResamplingMethod resampling,
    const GDALTransformerFunc &pfnTransform,
    gdal::dataset_unique_ptr &hSrcDS, gdal::warp_options_unique_ptr &psWarpOptions )
{
  // Open input file
  GDALAllRegister();
  hSrcDS.reset( GDALOpen( input.toUtf8().constData(), GA_ReadOnly ) );
  if ( !hSrcDS )
    return false;

  // Setup warp options.
  psWarpOptions.reset( GDALCreateWarpOptions() );
  psWarpOptions->hSrcDS = hSrcDS.get();
  psWarpOptions->nBandCount = GDALGetRasterCount( hSrcDS.get() );
  psWarpOptions->panSrcBands =
    ( int * ) CPLMalloc( sizeof( int ) * psWarpOptions->nBandCount );
  psWarpOptions->panDstBands =
    ( int * ) CPLMalloc( sizeof( int ) * psWarpOptions->nBandCount );
  for ( int i = 0; i < psWarpOptions->nBandCount; ++i )
  {
    psWarpOptions->panSrcBands[i] = i + 1;
    psWarpOptions->panDstBands[i] = i + 1;
  }
  psWarpOptions->pfnProgress = GDALTermProgress;
  psWarpOptions->pfnTransformer = pfnTransform;
  psWarpOptions->eResampleAlg = toGDALResampleAlg( resampling );

  return true;
}

bool QgsImageWarper::createDestinationDataset( const QString &outputName, GDALDatasetH hSrcDS, gdal::dataset_unique_ptr &hDstDS,
    uint resX, uint resY, double *adfGeoTransform, bool useZeroAsTrans,
    const QString &compression, const QgsCoordinateReferenceSystem &crs )
{
  // create the output file
  GDALDriverH driver = GDALGetDriverByName( "GTiff" );
  if ( !driver )
  {
    return false;
  }
  char **papszOptions = nullptr;
  papszOptions = CSLSetNameValue( papszOptions, "COMPRESS", compression.toLatin1() );
  hDstDS.reset( GDALCreate( driver,
                            outputName.toUtf8().constData(), resX, resY,
                            GDALGetRasterCount( hSrcDS ),
                            GDALGetRasterDataType( GDALGetRasterBand( hSrcDS, 1 ) ),
                            papszOptions ) );
  if ( !hDstDS )
  {
    return false;
  }

  if ( CE_None != GDALSetGeoTransform( hDstDS.get(), adfGeoTransform ) )
  {
    return false;
  }

  if ( crs.isValid() )
  {
    OGRSpatialReference oTargetSRS;
    oTargetSRS.importFromWkt( crs.toWkt( QgsCoordinateReferenceSystem::WKT_PREFERRED_GDAL ).toUtf8().data() );

    char *wkt = nullptr;
    const OGRErr err = oTargetSRS.exportToWkt( &wkt );
    if ( err != CE_None || GDALSetProjection( hDstDS.get(), wkt ) != CE_None )
    {
      CPLFree( wkt );
      return false;
    }
    CPLFree( wkt );
  }

  for ( int i = 0; i < GDALGetRasterCount( hSrcDS ); ++i )
  {
    GDALRasterBandH hSrcBand = GDALGetRasterBand( hSrcDS, i + 1 );
    GDALRasterBandH hDstBand = GDALGetRasterBand( hDstDS.get(), i + 1 );
    GDALColorTableH cTable = GDALGetRasterColorTable( hSrcBand );
    GDALSetRasterColorInterpretation( hDstBand, GDALGetRasterColorInterpretation( hSrcBand ) );
    if ( cTable )
    {
      GDALSetRasterColorTable( hDstBand, cTable );
    }

    int success;
    const double noData = GDALGetRasterNoDataValue( hSrcBand, &success );
    if ( success )
    {
      GDALSetRasterNoDataValue( hDstBand, noData );
    }
    else if ( useZeroAsTrans )
    {
      GDALSetRasterNoDataValue( hDstBand, 0 );
    }
  }

  return true;
}

int QgsImageWarper::warpFile( const QString &input,
                              const QString &output,
                              const QgsGeorefTransform &georefTransform,
                              ResamplingMethod resampling,
                              bool useZeroAsTrans,
                              const QString &compression,
                              const QgsCoordinateReferenceSystem &crs,
                              double destResX, double destResY )
{
  if ( !georefTransform.parametersInitialized() )
    return false;

  CPLErr eErr;
  gdal::dataset_unique_ptr hSrcDS;
  gdal::dataset_unique_ptr hDstDS;
  gdal::warp_options_unique_ptr psWarpOptions;
  if ( !openSrcDSAndGetWarpOpt( input, resampling, georefTransform.GDALTransformer(), hSrcDS, psWarpOptions ) )
  {
    // TODO: be verbose about failures
    return false;
  }

  double adfGeoTransform[6];
  int destPixels, destLines;
  eErr = GDALSuggestedWarpOutput( hSrcDS.get(), georefTransform.GDALTransformer(),
                                  georefTransform.GDALTransformerArgs(),
                                  adfGeoTransform, &destPixels, &destLines );
  if ( eErr != CE_None )
  {
    return false;
  }

  // If specified, override the suggested resolution with user values
  if ( destResX != 0.0 || destResY != 0.0 )
  {
    // If only one scale has been specified, fill in the other from the GDAL suggestion
    if ( destResX == 0.0 )
      destResX = adfGeoTransform[1];
    if ( destResY == 0.0 )
      destResY = adfGeoTransform[5];

    // Make sure user-specified coordinate system has canonical orientation
    if ( destResX < 0.0 )
      destResX = -destResX;
    if ( destResY > 0.0 )
      destResY = -destResY;

    // Assert that the north-up convention is fulfilled by GDALSuggestedWarpOutput (should always be the case)
    // Asserts are bad as they just crash out, changed to just return false. TS
    if ( adfGeoTransform[0] <= 0.0  || adfGeoTransform[5] >= 0.0 )
    {
      QgsDebugMsg( QStringLiteral( "Image is not north up after GDALSuggestedWarpOutput, bailing out." ) );
      return false;
    }
    // Find suggested output image extent (in georeferenced units)
    const double minX = adfGeoTransform[0];
    const double maxX = adfGeoTransform[0] + adfGeoTransform[1] * destPixels;
    const double maxY = adfGeoTransform[3];
    const double minY = adfGeoTransform[3] + adfGeoTransform[5] * destLines;

    // Update line and pixel count to match extent at user-specified resolution
    destPixels = ( int )( ( ( maxX - minX ) / destResX ) + 0.5 );
    destLines  = ( int )( ( ( minY - maxY ) / destResY ) + 0.5 );
    adfGeoTransform[0] = minX;
    adfGeoTransform[3] = maxY;
    adfGeoTransform[1] = destResX;
    adfGeoTransform[5] = destResY;
  }

  if ( !createDestinationDataset( output, hSrcDS.get(), hDstDS, destPixels, destLines,
                                  adfGeoTransform, useZeroAsTrans, compression,
                                  crs ) )
  {
    return false;
  }

  // Create a QT progress dialog
  QProgressDialog *progressDialog = new QProgressDialog( mParent );
  progressDialog->setWindowTitle( tr( "Progress Indication" ) );
  progressDialog->setRange( 0, 100 );
  progressDialog->setAutoClose( true );
  progressDialog->setModal( true );
  progressDialog->setMinimumDuration( 0 );

  // Set GDAL callbacks for the progress dialog
  psWarpOptions->pProgressArg = createWarpProgressArg( progressDialog );
  psWarpOptions->pfnProgress  = updateWarpProgress;

  psWarpOptions->hSrcDS = hSrcDS.get();
  psWarpOptions->hDstDS = hDstDS.get();

  // Create a transformer which transforms from source to destination pixels (and vice versa)
  psWarpOptions->pfnTransformer  = GeoToPixelTransform;
  psWarpOptions->pTransformerArg = addGeoToPixelTransform( georefTransform.GDALTransformer(),
                                   georefTransform.GDALTransformerArgs(),
                                   adfGeoTransform );

  // Initialize and execute the warp operation.
  GDALWarpOperation oOperation;
  oOperation.Initialize( psWarpOptions.get() );

  progressDialog->show();
  progressDialog->raise();
  progressDialog->activateWindow();

  eErr = oOperation.ChunkAndWarpImage( 0, 0, destPixels, destLines );

  destroyGeoToPixelTransform( psWarpOptions->pTransformerArg );
  delete progressDialog;
  return sWarpCanceled ? -1 : eErr == CE_None ? 1 : 0;
}


void *QgsImageWarper::addGeoToPixelTransform( GDALTransformerFunc GDALTransformer, void *GDALTransformerArg, double *padfGeotransform ) const
{
  TransformChain *chain = new TransformChain;
  chain->GDALTransformer = GDALTransformer;
  chain->GDALTransformerArg = GDALTransformerArg;
  memcpy( chain->adfGeotransform, padfGeotransform, sizeof( double ) * 6 );
  // TODO: In reality we don't require the full homogeneous matrix, so GeoToPixelTransform and matrix inversion could
  // be optimized for simple scale+offset if there's the need (e.g. for numerical or performance reasons).
  if ( !GDALInvGeoTransform( chain->adfGeotransform, chain->adfInvGeotransform ) )
  {
    // Error handling if inversion fails - although the inverse transform is not needed for warp operation
    delete chain;
    return nullptr;
  }
  return ( void * )chain;
}

void QgsImageWarper::destroyGeoToPixelTransform( void *GeoToPixelTransformArg ) const
{
  delete static_cast<TransformChain *>( GeoToPixelTransformArg );
}

int QgsImageWarper::GeoToPixelTransform( void *pTransformerArg, int bDstToSrc, int nPointCount,
    double *x, double *y, double *z, int *panSuccess )
{
  TransformChain *chain = static_cast<TransformChain *>( pTransformerArg );
  if ( !chain )
  {
    return false;
  }

  if ( !bDstToSrc )
  {
    // Transform to georeferenced coordinates
    if ( !chain->GDALTransformer( chain->GDALTransformerArg, bDstToSrc, nPointCount, x, y, z, panSuccess ) )
    {
      return false;
    }
    // Transform from georeferenced to pixel/line
    for ( int i = 0; i < nPointCount; ++i )
    {
      if ( !panSuccess[i] )
        continue;
      const double xP = x[i];
      const double yP = y[i];
      x[i] = chain->adfInvGeotransform[0] + xP * chain->adfInvGeotransform[1] + yP * chain->adfInvGeotransform[2];
      y[i] = chain->adfInvGeotransform[3] + xP * chain->adfInvGeotransform[4] + yP * chain->adfInvGeotransform[5];
    }
  }
  else
  {
    // Transform from pixel/line to georeferenced coordinates
    for ( int i = 0; i < nPointCount; ++i )
    {
      const double P = x[i];
      const double L = y[i];
      x[i] = chain->adfGeotransform[0] + P * chain->adfGeotransform[1] + L * chain->adfGeotransform[2];
      y[i] = chain->adfGeotransform[3] + P * chain->adfGeotransform[4] + L * chain->adfGeotransform[5];
    }
    // Transform from georeferenced coordinates to source pixel/line
    if ( !chain->GDALTransformer( chain->GDALTransformerArg, bDstToSrc, nPointCount, x, y, z, panSuccess ) )
    {
      return false;
    }
  }
  return true;
}

void *QgsImageWarper::createWarpProgressArg( QProgressDialog *progressDialog ) const
{
  return ( void * )progressDialog;
}

int CPL_STDCALL QgsImageWarper::updateWarpProgress( double dfComplete, const char *pszMessage, void *pProgressArg )
{
  Q_UNUSED( pszMessage )
  QProgressDialog *progress = static_cast<QProgressDialog *>( pProgressArg );
  progress->setValue( std::min( 100u, ( uint )( dfComplete * 100.0 ) ) );
  qApp->processEvents();
  // TODO: call QEventLoop manually to make "cancel" button more responsive
  if ( progress->wasCanceled() )
  {
    sWarpCanceled = true;
    return false;
  }

  sWarpCanceled = false;
  return true;
}

GDALResampleAlg QgsImageWarper::toGDALResampleAlg( const QgsImageWarper::ResamplingMethod method ) const
{
  switch ( method )
  {
    case NearestNeighbour:
      return GRA_NearestNeighbour;
    case Bilinear:
      return  GRA_Bilinear;
    case Cubic:
      return GRA_Cubic;
    case CubicSpline:
      return GRA_CubicSpline;
    case Lanczos:
      return GRA_Lanczos;
    default:
      break;
  };

  //avoid warning
  return GRA_NearestNeighbour;
}
