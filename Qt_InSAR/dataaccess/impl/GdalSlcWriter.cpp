#include "GdalSlcWriter.h"

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_port.h>
#include <cpl_string.h>
#include <QVector>

GdalSlcWriter::GdalSlcWriter() = default;
GdalSlcWriter::~GdalSlcWriter() { close(); }

bool GdalSlcWriter::create(const QString& filePath,
    int width, int height, int bandCount,
    const QString& projection)
{
    close();

    GDALDriverH driver = GDALGetDriverByName("GTiff");
    if (!driver) {
        mLastError = QStringLiteral("GTiff 驱动不可用");
        return false;
    }

    GDALDatasetH hDS = GDALCreate(driver,
        filePath.toUtf8().constData(),
        width, height, bandCount,
        GDT_CFloat32, nullptr);

    if (!hDS) {
        mLastError = QStringLiteral("无法创建输出文件: %1").arg(filePath);
        return false;
    }

    // 告知 GDAL/QGIS 按幅度渲染复数数据
    GDALSetMetadataItem(hDS, "COMPLEX_SCHEME", "MAGNITUDE", "IMAGE_STRUCTURE");

    if (!projection.isEmpty()) {
        GDALSetProjection(hDS, projection.toUtf8().constData());
    }

    mDataset = hDS;
    return true;
}

bool GdalSlcWriter::writeBand(int bandIndex,
    const QVector<std::complex<float>>& data)
{
    if (!mDataset) {
        mLastError = QStringLiteral("数据集未创建");
        return false;
    }

    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    int w = ds->GetRasterXSize();
    int h = ds->GetRasterYSize();
    int expectedSize = w * h;

    if (data.size() < expectedSize) {
        mLastError = QStringLiteral("数据大小不足: 期望 %1, 实际 %2")
            .arg(expectedSize).arg(data.size());
        return false;
    }

    GDALRasterBand* band = ds->GetRasterBand(bandIndex + 1);
    CPLErr err = band->RasterIO(GF_Write, 0, 0, w, h,
        const_cast<std::complex<float>*>(data.constData()),
        w, h, GDT_CFloat32, 0, 0);

    if (err != CE_None) {
        mLastError = QStringLiteral("写入波段 %1 失败").arg(bandIndex);
        return false;
    }

    return true;
}

bool GdalSlcWriter::writeRow(int row,
    const QVector<std::complex<float>>& data)
{
    if (!mDataset) {
        mLastError = QStringLiteral("数据集未创建");
        return false;
    }
    GDALDataset* ds = static_cast<GDALDataset*>(mDataset);
    int w = ds->GetRasterXSize();
    int h = ds->GetRasterYSize();
    if (row < 0 || row >= h) return false;
    if (data.size() < w) return false;

    GDALRasterBand* band = ds->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Write, 0, row, w, 1,
        const_cast<std::complex<float>*>(data.constData()),
        w, 1, GDT_CFloat32, 0, 0);

    if (err != CE_None) {
        mLastError = QStringLiteral("写入行 %1 失败").arg(row);
        return false;
    }
    return true;
}

void GdalSlcWriter::setGeoTransform(double x0, double dx, double rx,
                                    double y0, double ry, double dy)
{
    if (!mDataset) return;
    double gt[6] = {x0, dx, rx, y0, ry, dy};
    GDALSetGeoTransform(static_cast<GDALDatasetH>(mDataset), gt);
}

void GdalSlcWriter::copyGeoreferencing(void* srcDataset, const QString& /*projection*/)
{
    if (!mDataset || !srcDataset) return;
    GDALDatasetH hSrc = static_cast<GDALDatasetH>(srcDataset);
    GDALDatasetH hDst = static_cast<GDALDatasetH>(mDataset);

    // 只复制 GCPs — SLC 通过地面控制点做地理定位, 不写 GeoTransform/Projection
    int gcpCount = GDALGetGCPCount(hSrc);
    if (gcpCount > 0) {
        GDALSetGCPs(hDst, gcpCount,
            GDALGetGCPs(hSrc), GDALGetGCPProjection(hSrc));
    }
}

void GdalSlcWriter::close()
{
    if (mDataset) {
        GDALClose(static_cast<GDALDatasetH>(mDataset));
        mDataset = nullptr;
    }
    mLastError.clear();
}

QString GdalSlcWriter::lastError() const { return mLastError; }
