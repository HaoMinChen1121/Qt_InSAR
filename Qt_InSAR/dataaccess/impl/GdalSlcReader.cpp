#include "GdalSlcReader.h"

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_port.h>
#include <QVector>
#include <algorithm>

GdalSlcReader::GdalSlcReader() = default;
GdalSlcReader::~GdalSlcReader() { close(); }

bool GdalSlcReader::open(const QString& filePath)
{
    close();

    GDALDatasetH hDS = GDALOpenShared(filePath.toUtf8().constData(), GA_ReadOnly);
    if (!hDS) return false;

    mDataset = hDS;
    mFilePath = filePath;
    mWidth = GDALGetRasterXSize(hDS);
    mHeight = GDALGetRasterYSize(hDS);
    mBandCount = GDALGetRasterCount(hDS);
    return true;
}

void GdalSlcReader::close()
{
    if (mDataset) {
        GDALClose(static_cast<GDALDatasetH>(mDataset));
        mDataset = nullptr;
    }
    mWidth = 0;
    mHeight = 0;
    mBandCount = 0;
    mFilePath.clear();
}

bool GdalSlcReader::isOpen() const { return mDataset != nullptr; }

QVector<std::complex<float>> GdalSlcReader::readBand(int bandIndex)
{
    if (!mDataset || bandIndex < 0 || bandIndex >= mBandCount)
        return {};

    QVector<std::complex<float>> buffer(mWidth * mHeight);
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    GDALRasterBand* band = ds->GetRasterBand(bandIndex + 1);

    CPLErr err = band->RasterIO(GF_Read, 0, 0, mWidth, mHeight,
        buffer.data(), mWidth, mHeight, GDT_CFloat32, 0, 0);

    if (err != CE_None)
        buffer.clear();
    return buffer;
}

QVector<std::complex<float>> GdalSlcReader::readBandWindow(int bandIndex,
    int colStart, int rowStart, int colSize, int rowSize)
{
    if (!mDataset || bandIndex < 0 || bandIndex >= mBandCount)
        return {};

    if (colStart < 0) { colSize += colStart; colStart = 0; }
    if (rowStart < 0) { rowSize += rowStart; rowStart = 0; }
    if (colStart + colSize > mWidth)  colSize = mWidth  - colStart;
    if (rowStart + rowSize > mHeight) rowSize = mHeight - rowStart;
    if (colSize <= 0 || rowSize <= 0)
        return {};

    QVector<std::complex<float>> buffer(colSize * rowSize);
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    GDALRasterBand* band = ds->GetRasterBand(bandIndex + 1);

    CPLErr err = band->RasterIO(GF_Read, colStart, rowStart,
        colSize, rowSize, buffer.data(), colSize, rowSize,
        GDT_CFloat32, 0, 0);

    if (err != CE_None)
        buffer.clear();
    return buffer;
}

int GdalSlcReader::width() const { return mWidth; }
int GdalSlcReader::height() const { return mHeight; }
int GdalSlcReader::bandCount() const { return mBandCount; }
QString GdalSlcReader::projection() const
{
    if (!mDataset) return {};
    const char* proj = GDALGetProjectionRef(
        static_cast<GDALDatasetH>(mDataset));
    return QString::fromUtf8(proj);
}
QString GdalSlcReader::filePath() const { return mFilePath; }

bool GdalSlcReader::geoTransform(double gt[6]) const
{
    if (!mDataset) return false;
    return GDALGetGeoTransform(static_cast<GDALDatasetH>(mDataset), gt) == CE_None;
}
