#include "IWMerger.h"
#include <gdal_priv.h>
#include <QDebug>
#include <QFileInfo>
#include <vector>
#include <algorithm>
#include <cstring>

namespace IWMerger {

template<typename T>
static bool readRow(GDALRasterBandH band, int row, int width, std::vector<T>& buf)
{
    buf.resize(width);
    GDALDataType dt = (sizeof(T) == sizeof(float)) ? GDT_Float32 : GDT_CFloat32;
    return GDALRasterIO(band, GF_Read, 0, row, width, 1,
               buf.data(), width, 1, dt, 0, 0) == CE_None;
}

template<typename T>
static bool writeRow(GDALRasterBandH band, int row, int width, const std::vector<T>& buf)
{
    GDALDataType dt = (sizeof(T) == sizeof(float)) ? GDT_Float32 : GDT_CFloat32;
    return GDALRasterIO(band, GF_Write, 0, row, width, 1,
               const_cast<T*>(buf.data()), width, 1, dt, 0, 0) == CE_None;
}

// 简单像素拼接: IW1 | IW2 | IW3, 每对边缘裁掉 overlapTrim 列
template<typename T>
static bool mergeT(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath,
    GDALDataType dataType,
    int overlapTrim)
{
    int nIW = iwFiles.size();
    if (nIW < 2) return false;

    int mH = iwMetas[0].height;
    // 计算合并宽度: sum(IW widths) - overlapTrim * (nIW-1)
    int mW = 0;
    for (int i = 0; i < nIW; ++i) {
        mW += iwMetas[i].width;
        mH = std::min(mH, iwMetas[i].height);
    }
    mW -= overlapTrim * (nIW - 1);

    QVector<GDALDatasetH> srcDS(nIW);
    QVector<GDALRasterBandH> srcBand(nIW);
    QVector<int> srcW(nIW);
    for (int i = 0; i < nIW; ++i) {
        srcDS[i] = GDALOpen(iwFiles[i].toUtf8().constData(), GA_ReadOnly);
        if (!srcDS[i]) { qWarning() << "[IWMerge] open fail:" << iwFiles[i]; return false; }
        srcBand[i] = GDALGetRasterBand(srcDS[i], 1);
        srcW[i] = iwMetas[i].width;
    }

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dstDS = GDALCreate(drv, outputPath.toUtf8().constData(),
        mW, mH, 1, dataType, nullptr);
    if (!dstDS) {
        for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
        return false;
    }
    // 设置 Identity geotransform (像素坐标)
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(dstDS, gt);
    GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);

    std::vector<T> outBuf(mW);
    std::vector<T> iwBuf;

    for (int row = 0; row < mH; ++row) {
        std::fill(outBuf.begin(), outBuf.end(), T{});
        int dstCol = 0;
        for (int iw = 0; iw < nIW; ++iw) {
            if (!readRow<T>(srcBand[iw], row, srcW[iw], iwBuf)) continue;
            int copyW = srcW[iw];
            // 对最后一张以外的 IW, 裁剪尾部 overlap
            if (iw < nIW - 1) copyW -= overlapTrim;
            if (copyW <= 0) continue;
            std::memcpy(outBuf.data() + dstCol, iwBuf.data(), copyW * sizeof(T));
            dstCol += copyW;
        }
        writeRow<T>(dstBand, row, mW, outBuf);
        if (row % 100 == 0) qDebug() << "[IWMerge] row" << row << "/" << mH;
    }

    GDALClose(dstDS);
    for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
    qDebug() << "[IWMerge] concat" << mW << "x" << mH << "->" << outputPath;
    return true;
}

static int computeOverlapTrim(const QVector<IwMeta>& metas)
{
    if (metas.size() < 2) return 0;
    // 从相邻IW的近距/远距计算重叠像素数
    double totalOverlap = 0.0;
    int pairs = 0;
    for (int i = 0; i < metas.size() - 1; ++i) {
        double leftFar = metas[i].nearRange + metas[i].width * metas[i].rangeSpacing;
        double rightNear = metas[i + 1].nearRange;
        double overlap = leftFar - rightNear;
        if (overlap > 0) {
            double avgSpacing = (metas[i].rangeSpacing + metas[i + 1].rangeSpacing) / 2.0;
            if (avgSpacing > 0) totalOverlap += overlap / avgSpacing;
            ++pairs;
        }
    }
    return pairs > 0 ? static_cast<int>(totalOverlap / pairs + 0.5) : 0;
}

bool mergePhase(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50; // 回退: S1 IW 典型重叠
    return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32, trim);
}
bool mergeCoherence(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50;
    return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32, trim);
}
bool mergeComplex(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50;
    return mergeT<std::complex<float>>(iwFiles, iwMetas, outputPath, GDT_CFloat32, trim);
}

} // namespace IWMerger
