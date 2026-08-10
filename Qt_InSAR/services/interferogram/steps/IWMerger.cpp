#include "IWMerger.h"
#include <gdal_priv.h>
#include <QDebug>
#include <QFileInfo>
#include <vector>
#include <cmath>
#include <algorithm>

namespace IWMerger {

// 根据 range 计算合并后的网格尺寸
static bool computeMergedGrid(const QVector<IwMeta>& metas,
    double* outNearR, int* outWidth, int* outHeight)
{
    if (metas.isEmpty()) return false;
    double nearR = metas[0].nearRange;
    double farR  = nearR + metas[0].width * metas[0].rangeSpacing;
    double spacing = metas[0].rangeSpacing;
    int h = metas[0].height;

    for (int i = 1; i < metas.size(); ++i) {
        nearR = std::min(nearR, metas[i].nearRange);
        double f = metas[i].nearRange + metas[i].width * metas[i].rangeSpacing;
        farR = std::max(farR, f);
        if (metas[i].height != h) {
            qWarning() << "[IWMerge] height mismatch" << h << "vs" << metas[i].height;
            h = std::min(h, metas[i].height);
        }
    }
    *outNearR  = nearR;
    *outWidth  = static_cast<int>(std::ceil((farR - nearR) / spacing));
    *outHeight = h;
    return true;
}

// 打开文件并读取一整行
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

// 核心拼接: 逐行读取各 IW, 按 range 映射到合并网格
template<typename T>
static bool mergeT(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath,
    GDALDataType dataType)
{
    int nIW = iwFiles.size();
    if (nIW != iwMetas.size()) return false;

    double nearR; int mW, mH;
    if (!computeMergedGrid(iwMetas, &nearR, &mW, &mH)) return false;
    double spacing = iwMetas[0].rangeSpacing;

    // 打开所有 IW 输入文件
    QVector<GDALDatasetH> srcDS(nIW);
    QVector<GDALRasterBandH> srcBand(nIW);
    for (int i = 0; i < nIW; ++i) {
        srcDS[i] = GDALOpen(iwFiles[i].toUtf8().constData(), GA_ReadOnly);
        if (!srcDS[i]) { qWarning() << "[IWMerge] open fail:" << iwFiles[i]; return false; }
        srcBand[i] = GDALGetRasterBand(srcDS[i], 1);
    }

    // 创建输出文件
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dstDS = GDALCreate(drv, outputPath.toUtf8().constData(),
        mW, mH, 1, dataType, nullptr);
    if (!dstDS) {
        for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
        return false;
    }
    GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);
    GDALSetRasterNoDataValue(dstBand, 0.0);

    // 为每个 IW 预计算 range 范围
    QVector<double> iwNear(nIW), iwFar(nIW);
    for (int i = 0; i < nIW; ++i) {
        iwNear[i] = iwMetas[i].nearRange;
        iwFar[i]  = iwNear[i] + iwMetas[i].width * iwMetas[i].rangeSpacing;
    }

    std::vector<T> outBuf(mW);
    std::vector<T> iwBuf;

    for (int row = 0; row < mH; ++row) {
        std::fill(outBuf.begin(), outBuf.end(), T{});
        for (int iw = 0; iw < nIW; ++iw) {
            // 该 IW 在合并网格中的列范围
            int c0 = static_cast<int>((iwNear[iw] - nearR) / spacing);
            int c1 = static_cast<int>((iwFar[iw]  - nearR) / spacing);
            if (c0 < 0) c0 = 0;
            if (c1 > mW) c1 = mW;
            if (c0 >= c1) continue;

            int iwW = iwMetas[iw].width;
            if (!readRow<T>(srcBand[iw], row, iwW, iwBuf)) continue;

            for (int c = c0; c < c1; ++c) {
                double R = nearR + c * spacing;
                double iwColD = (R - iwNear[iw]) / iwMetas[iw].rangeSpacing;
                int iwCol = static_cast<int>(std::floor(iwColD + 0.5));
                if (iwCol < 0 || iwCol >= iwW) continue;
                // 如果此像素已被另一个 IW 填充 (overlap), 取 iwCol 更靠近中心的 IW
                if (outBuf[c] != T{}) {
                    int prevDist = std::abs(iwCol - iwW / 2);
                    (void)prevDist; // 简化: 后来者覆盖
                }
                outBuf[c] = iwBuf[iwCol];
            }
        }
        writeRow<T>(dstBand, row, mW, outBuf);
        if (row % 100 == 0) qDebug() << "[IWMerge] row" << row << "/" << mH;
    }

    GDALClose(dstDS);
    for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
    qDebug() << "[IWMerge] output:" << outputPath << mW << "x" << mH;
    return true;
}

bool mergePhase(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32);
}
bool mergeCoherence(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32);
}
bool mergeComplex(const QVector<QString>& iwFiles, const QVector<IwMeta>& iwMetas,
    const QString& outputPath) {
    return mergeT<std::complex<float>>(iwFiles, iwMetas, outputPath, GDT_CFloat32);
}

} // namespace IWMerger
