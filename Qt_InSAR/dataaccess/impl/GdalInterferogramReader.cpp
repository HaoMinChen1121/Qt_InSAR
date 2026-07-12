#include "GdalInterferogramReader.h"
#include <gdal_priv.h>
#include <QVector>

GdalInterferogramReader::GdalInterferogramReader() = default;
GdalInterferogramReader::~GdalInterferogramReader() { close(); }

bool GdalInterferogramReader::open(const QString& filePath)
{
    close();
    GDALDatasetH hDS = GDALOpen(filePath.toUtf8().constData(), GA_ReadOnly);
    if (!hDS) return false;
    mDataset = hDS;
    mFilePath = filePath;
    mWidth  = GDALGetRasterXSize(hDS);
    mHeight = GDALGetRasterYSize(hDS);
    return true;
}

void GdalInterferogramReader::close()
{
    if (mDataset) { GDALClose(static_cast<GDALDatasetH>(mDataset)); mDataset = nullptr; }
    mWidth = 0; mHeight = 0; mFilePath.clear();
}

bool GdalInterferogramReader::isOpen() const { return mDataset != nullptr; }

QVector<std::complex<float>> GdalInterferogramReader::readComplex()
{
    if (!mDataset) return {};
    QVector<std::complex<float>> buf(mWidth * mHeight);
    GDALRasterBandH band = GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 1);
    CPLErr err = GDALRasterIO(band, GF_Read, 0, 0, mWidth, mHeight,
        buf.data(), mWidth, mHeight, GDT_CFloat32, 0, 0);
    if (err != CE_None) buf.clear();
    return buf;
}

QVector<float> GdalInterferogramReader::readPhase()
{
    if (!mDataset) return {};
    QVector<float> buf(mWidth * mHeight);
    GDALRasterBandH band = GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 2);
    if (!band) { buf.fill(0); return buf; }
    GDALRasterIO(band, GF_Read, 0, 0, mWidth, mHeight,
        buf.data(), mWidth, mHeight, GDT_Float32, 0, 0);
    return buf;
}

QVector<float> GdalInterferogramReader::readCoherence()
{
    if (!mDataset) return {};
    QVector<float> buf(mWidth * mHeight);
    GDALRasterBandH band = GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 3);
    if (!band) { buf.fill(0); return buf; }
    GDALRasterIO(band, GF_Read, 0, 0, mWidth, mHeight,
        buf.data(), mWidth, mHeight, GDT_Float32, 0, 0);
    return buf;
}

int     GdalInterferogramReader::width() const { return mWidth; }
int     GdalInterferogramReader::height() const { return mHeight; }
QString GdalInterferogramReader::filePath() const { return mFilePath; }
