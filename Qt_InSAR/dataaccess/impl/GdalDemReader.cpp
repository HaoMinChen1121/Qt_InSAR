#include "GdalDemReader.h"
#include <gdal_priv.h>
#include <cpl_conv.h>
#include <QVector>
#include <cstring>

GdalDemReader::GdalDemReader() = default;
GdalDemReader::~GdalDemReader() { close(); }

bool GdalDemReader::open(const QString& filePath)
{
    close();
    GDALDatasetH hDS = GDALOpen(filePath.toUtf8().constData(), GA_ReadOnly);
    if (!hDS) return false;

    mDataset = hDS;
    mWidth  = GDALGetRasterXSize(hDS);
    mHeight = GDALGetRasterYSize(hDS);
    GDALGetGeoTransform(hDS, mGeoTransform);
    mNoData = GDALGetRasterNoDataValue(GDALGetRasterBand(hDS, 1), nullptr);
    return true;
}

void GdalDemReader::close()
{
    if (mDataset) { GDALClose(static_cast<GDALDatasetH>(mDataset)); mDataset = nullptr; }
    mWidth = 0; mHeight = 0;
    std::memset(mGeoTransform, 0, sizeof(mGeoTransform));
}

bool GdalDemReader::isOpen() const { return mDataset != nullptr; }

QVector<float> GdalDemReader::readElevation()
{
    if (!mDataset) return {};
    QVector<float> buf(mWidth * mHeight);
    GDALRasterBandH band = GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 1);
    GDALRasterIO(band, GF_Read, 0, 0, mWidth, mHeight,
        buf.data(), mWidth, mHeight, GDT_Float32, 0, 0);
    return buf;
}

QVector<float> GdalDemReader::readElevationWindow(
    int colStart, int rowStart, int colSize, int rowSize)
{
    if (!mDataset) return {};
    if (colStart < 0) { colSize += colStart; colStart = 0; }
    if (rowStart < 0) { rowSize += rowStart; rowStart = 0; }
    if (colStart + colSize > mWidth)  colSize = mWidth  - colStart;
    if (rowStart + rowSize > mHeight) rowSize = mHeight - rowStart;
    if (colSize <= 0 || rowSize <= 0) return {};

    QVector<float> buf(colSize * rowSize);
    GDALRasterBandH band = GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 1);
    GDALRasterIO(band, GF_Read, colStart, rowStart, colSize, rowSize,
        buf.data(), colSize, rowSize, GDT_Float32, 0, 0);
    return buf;
}

int     GdalDemReader::width() const { return mWidth; }
int     GdalDemReader::height() const { return mHeight; }
double* GdalDemReader::geoTransform() const { return const_cast<double*>(mGeoTransform); }
QString GdalDemReader::projection() const
{
    if (!mDataset) return {};
    return QString::fromUtf8(GDALGetProjectionRef(static_cast<GDALDatasetH>(mDataset)));
}
double  GdalDemReader::noDataValue() const { return mNoData; }
