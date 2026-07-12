#include "GdalInterferogramWriter.h"
#include <gdal_priv.h>
#include <QVector>

GdalInterferogramWriter::GdalInterferogramWriter() = default;
GdalInterferogramWriter::~GdalInterferogramWriter() { close(); }

bool GdalInterferogramWriter::create(const QString& filePath,
    int width, int height, bool isComplex)
{
    close();
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    if (!driver) { mLastError = "GTiff driver unavailable"; return false; }

    // Band 1: always CFloat32 for complex interferogram
    // Band 2: Float32 for phase (radians)
    // Band 3: Float32 for coherence (0-1)
    int nBands = isComplex ? 3 : 2;
    GDALDataType type1 = isComplex ? GDT_CFloat32 : GDT_Float32;

    GDALDatasetH hDS = GDALCreate(driver, filePath.toUtf8().constData(),
        width, height, nBands, type1, nullptr);
    if (!hDS) { mLastError = "Cannot create: " + filePath; return false; }

    if (nBands >= 2) {
        // Add Float32 bands for phase and coherence
        for (int b = 2; b <= nBands; ++b) {
            GDALAddBand(hDS, GDT_Float32, nullptr);
        }
    }

    // Default geotransform
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(hDS, gt);

    mDataset = hDS;
    return true;
}

bool GdalInterferogramWriter::writeComplex(const QVector<std::complex<float>>& data)
{
    if (!mDataset) { mLastError = "Dataset not created"; return false; }
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    int w = ds->GetRasterXSize(), h = ds->GetRasterYSize();
    if (data.size() < w * h) return false;
    GDALRasterBand* band = ds->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, w, h,
        const_cast<std::complex<float>*>(data.constData()),
        w, h, GDT_CFloat32, 0, 0);
    return err == CE_None;
}

bool GdalInterferogramWriter::writePhase(const QVector<float>& data)
{
    if (!mDataset) { mLastError = "Dataset not created"; return false; }
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    int w = ds->GetRasterXSize(), h = ds->GetRasterYSize();
    if (data.size() < w * h) return false;
    GDALRasterBand* band = ds->GetRasterBand(2);
    if (!band) return false;
    CPLErr err = band->RasterIO(GF_Write, 0, 0, w, h,
        const_cast<float*>(data.constData()), w, h, GDT_Float32, 0, 0);
    return err == CE_None;
}

bool GdalInterferogramWriter::writeCoherence(const QVector<float>& data)
{
    if (!mDataset) { mLastError = "Dataset not created"; return false; }
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    int w = ds->GetRasterXSize(), h = ds->GetRasterYSize();
    if (data.size() < w * h) return false;
    GDALRasterBand* band = ds->GetRasterBand(3);
    if (!band) return false;
    CPLErr err = band->RasterIO(GF_Write, 0, 0, w, h,
        const_cast<float*>(data.constData()), w, h, GDT_Float32, 0, 0);
    return err == CE_None;
}

void GdalInterferogramWriter::close()
{
    if (mDataset) { GDALClose(static_cast<GDALDatasetH>(mDataset)); mDataset = nullptr; }
}
QString GdalInterferogramWriter::lastError() const { return mLastError; }
