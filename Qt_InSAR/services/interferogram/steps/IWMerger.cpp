#include "IWMerger.h"
#include "domain/SarComplexTypes.h"
#include <gdal_priv.h>
#include <QDebug>
#include <QFileInfo>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <complex>

namespace IWMerger {

// 相位旋转: float = 相位减法回绕; complex = 复数旋转
template<typename T> T rotatePixel(T v, double ph);

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

// 相邻 IW 重叠像素数 (全分辨率几何, 换算到输出列)
static int computeOverlapTrim(const QVector<IwMeta>& metas)
{
    if (metas.size() < 2) return 0;
    double totalOverlap = 0.0;
    int pairs = 0;
    for (int i = 0; i < metas.size() - 1; ++i) {
        int fullW = metas[i].fullResWidth > 0
            ? metas[i].fullResWidth : metas[i].width * std::max(1, metas[i].rangeLooks);
        double leftFar = metas[i].nearRange + fullW * metas[i].rangeSpacing;
        double rightNear = metas[i + 1].nearRange;
        double overlap = leftFar - rightNear;
        if (overlap > 0) {
            double avgSpacing = (metas[i].rangeSpacing + metas[i + 1].rangeSpacing) / 2.0;
            if (avgSpacing > 0) { totalOverlap += overlap / avgSpacing; ++pairs; }
        }
    }
    if (pairs <= 0) return 0;
    int fullRes = static_cast<int>(totalOverlap / pairs + 0.5);
    int rgLooks = std::max(1, metas[0].rangeLooks);
    return std::max(1, (fullRes + rgLooks / 2) / rgLooks);
}

// ═══════════════════════════════════════════════════════════
//  子条带相位一致性对齐 (常数+线性, 鲁棒估计 — 设计 §5.3)
//  重叠区 = A 尾 trim 列 / B 头 trim 列 (同一地面范围)
// ═══════════════════════════════════════════════════════════

struct AlignResult {
    bool ok = false;
    double a = 0;        // 常数 (rad)
    double b = 0;        // 线性 (rad/列, 验收后)
    double rawB = 0;     // 线性 (rad/列, 验收前, 诊断用)
    double meanCoh = 0;
    double r2 = 0;
};

static AlignResult estimateSeamAlignment(
    GDALRasterBandH phA, GDALRasterBandH cohA, int wA, int hA, int offA,
    GDALRasterBandH phB, GDALRasterBandH cohB, int wB, int hB, int offB,
    int trim)
{
    AlignResult r;
    const int row0 = std::max(offA, offB);
    const int row1 = std::min(offA + hA, offB + hB);
    if (row1 - row0 < 50 || trim < 4) return r;

    std::vector<double> re(trim, 0.0), im(trim, 0.0), wcol(trim, 0.0);
    std::vector<float> rowA(static_cast<size_t>(wA)), rowB(static_cast<size_t>(wB));
    std::vector<float> cA(static_cast<size_t>(wA)), cB(static_cast<size_t>(wB));
    double totalW = 0;
    long totalN = 0;

    for (int row = row0; row < row1; ++row) {
        const int ra = row - offA;
        const int rb = row - offB;
        GDALRasterIO(phA, GF_Read, 0, ra, wA, 1, rowA.data(), wA, 1, GDT_Float32, 0, 0);
        GDALRasterIO(phB, GF_Read, 0, rb, wB, 1, rowB.data(), wB, 1, GDT_Float32, 0, 0);
        if (cohA) GDALRasterIO(cohA, GF_Read, 0, ra, wA, 1, cA.data(), wA, 1, GDT_Float32, 0, 0);
        if (cohB) GDALRasterIO(cohB, GF_Read, 0, rb, wB, 1, cB.data(), wB, 1, GDT_Float32, 0, 0);
        for (int c = 0; c < trim; ++c) {
            const int colA = wA - trim + c;
            double w = 1.0;
            if (cohA && cohB) w = std::min(cA[colA], cB[c]);
            if (w < 0.05) continue;
            const double d = rowB[c] - rowA[colA];
            re[c] += w * std::cos(d);
            im[c] += w * std::sin(d);
            wcol[c] += w;
            totalW += w;
            ++totalN;
        }
    }

    // 列向相位差 + unwrap
    std::vector<double> dphi(trim, 0.0);
    int valid = 0;
    for (int c = 0; c < trim; ++c) {
        if (wcol[c] <= 0) { dphi[c] = 0; continue; }
        dphi[c] = std::atan2(im[c], re[c]);
        ++valid;
    }
    if (valid < trim / 2) return r;
    for (int c = 1; c < trim; ++c) {
        if (wcol[c] <= 0 || wcol[c - 1] <= 0) { dphi[c] = dphi[c - 1]; continue; }
        while (dphi[c] - dphi[c - 1] > M_PI)  dphi[c] -= 2 * M_PI;
        while (dphi[c] - dphi[c - 1] < -M_PI) dphi[c] += 2 * M_PI;
    }

    r.meanCoh = totalN > 0 ? totalW / totalN : 0.0;

    // 加权线性拟合 Δφ ≈ a + b·c
    double s0 = 0, s1 = 0, s2 = 0, sy0 = 0, sy1 = 0;
    for (int c = 0; c < trim; ++c) {
        if (wcol[c] <= 0) continue;
        double w = wcol[c], x = c, y = dphi[c];
        s0 += w; s1 += w * x; s2 += w * x * x;
        sy0 += w * y; sy1 += w * x * y;
    }
    double denom = s0 * s2 - s1 * s1;
    if (std::abs(denom) < 1e-12) { r.a = sy0 / std::max(s0, 1e-12); r.ok = true; return r; }
    const double a = (sy0 * s2 - sy1 * s1) / denom;
    const double b = (s0 * sy1 - s1 * sy0) / denom;

    // R²
    double ssTot = 0, ssRes = 0, yMean = 0;
    for (int c = 0; c < trim; ++c)
        if (wcol[c] > 0) yMean += wcol[c] * dphi[c];
    yMean /= std::max(s0, 1e-12);
    for (int c = 0; c < trim; ++c) {
        if (wcol[c] <= 0) continue;
        double e = dphi[c] - (a + b * c);
        ssRes += wcol[c] * e * e;
        ssTot += wcol[c] * (dphi[c] - yMean) * (dphi[c] - yMean);
    }
    r.r2 = ssTot > 1e-12 ? 1.0 - ssRes / ssTot : 0.0;

    // 验收降级 (设计 §5.3): 低相干/低 R²/斜率过大 → 纯常数
    // 注: ① coh 门限 0.1 (实测 0.15 时 R² 仍可达 0.9, 0.2 过于保守)
    //     ② 斜率上限按"每列"而非"总跨度": 平地相位梯度实测 ~35°/列
    //        (R² 0.9 的真实线性信号), 总跨度截断 π 会误杀全部线性项
    r.a = a; r.b = b; r.rawB = b;
    if (r.meanCoh < 0.1) r.b = 0;
    else if (r.r2 < 0.6) r.b = 0;
    else if (std::abs(b) > M_PI / 2.0) r.b = 0;   // 每列 >90° (unwrap 安全上限)
    r.ok = true;
    return r;
}

// ═══════════════════════════════════════════════════════════
//  像素拼接: IW1 | IW2 | IW3
//  - 距离向: 每对边缘裁 overlapTrim 列
//  - 方位向: azimuthOffset 行偏移 + 裁剪到公共方位范围
// ═══════════════════════════════════════════════════════════

// 公共方位范围 (输出行坐标)
static void commonExtent(const QVector<IwMeta>& metas, int* top, int* bottom)
{
    *top = 0; *bottom = 0;
    for (int i = 0; i < metas.size(); ++i) {
        if (i == 0 || metas[i].azimuthOffset > *top)
            *top = metas[i].azimuthOffset;
        int b = metas[i].azimuthOffset + metas[i].height;
        if (i == 0 || b < *bottom)
            *bottom = b;
    }
    *bottom = std::max(*bottom, *top);
}

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

    int mW = 0;
    for (int i = 0; i < nIW; ++i)
        mW += iwMetas[i].width
            - (i > 0 ? overlapTrim : 0)
            - (i < nIW - 1 ? overlapTrim : 0);

    int topOff = 0, bottom = 0;
    commonExtent(iwMetas, &topOff, &bottom);
    const int mH = bottom - topOff;

    QVector<GDALDatasetH> srcDS(nIW);
    QVector<GDALRasterBandH> srcBand(nIW);
    QVector<int> srcW(nIW);
    QVector<int> srcOff(nIW);
    for (int i = 0; i < nIW; ++i) {
        srcDS[i] = GDALOpen(iwFiles[i].toUtf8().constData(), GA_ReadOnly);
        if (!srcDS[i]) { qWarning() << "[IWMerge] open fail:" << iwFiles[i]; return false; }
        srcBand[i] = GDALGetRasterBand(srcDS[i], 1);
        srcW[i] = iwMetas[i].width;
        srcOff[i] = iwMetas[i].azimuthOffset;
    }

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dstDS = GDALCreate(drv, outputPath.toUtf8().constData(),
        mW, mH, 1, dataType, nullptr);
    if (!dstDS) {
        for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
        return false;
    }
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(dstDS, gt);
    GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);

    std::vector<T> outBuf(mW);
    std::vector<T> iwBuf;

    for (int row = topOff; row < bottom; ++row) {
        std::fill(outBuf.begin(), outBuf.end(), T{});
        int dstCol = 0;
        for (int iw = 0; iw < nIW; ++iw) {
            const int srcRow = row - srcOff[iw];
            // 重叠区两侧都裁剪: 子条带边缘 = 天线滚降带 (幅度低 → 黑带)
            const int colOff = (iw > 0 ? overlapTrim : 0);
            const int copyW = srcW[iw] - colOff - (iw < nIW - 1 ? overlapTrim : 0);
            if (srcRow >= 0 && srcRow < iwMetas[iw].height
                && readRow<T>(srcBand[iw], srcRow, srcW[iw], iwBuf)) {
                std::memcpy(outBuf.data() + dstCol, iwBuf.data() + colOff, copyW * sizeof(T));
            }
            dstCol += copyW;
        }
        writeRow<T>(dstBand, row - topOff, mW, outBuf);
        if ((row - topOff) % 100 == 0) qDebug() << "[IWMerge] row" << (row - topOff) << "/" << mH;
    }

    GDALClose(dstDS);
    for (int i = 0; i < nIW; ++i) GDALClose(srcDS[i]);
    qDebug() << "[IWMerge] concat" << mW << "x" << mH << "->" << outputPath;
    return true;
}

// 带子条带相位对齐的拼接 (phase / 复数 ifg)
// alignPhFiles: 对齐估计的相位源 (复数输出必须用相位文件估计 —
//                GDAL CFloat32→Float32 转换取模而非相位)
template<typename T>
static bool mergeAlignedT(
    const QVector<QString>& iwFiles,
    const QVector<QString>& cohFiles,        // 相干性权重 (可为空)
    const QVector<QString>& alignPhFiles,    // 对齐估计相位源 (与 iwFiles 同序)
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath,
    GDALDataType dataType,
    int overlapTrim)
{
    int nIW = iwFiles.size();
    if (nIW < 2) return false;

    int mW = 0;
    for (int i = 0; i < nIW; ++i)
        mW += iwMetas[i].width
            - (i > 0 ? overlapTrim : 0)
            - (i < nIW - 1 ? overlapTrim : 0);

    int topOff = 0, bottom = 0;
    commonExtent(iwMetas, &topOff, &bottom);
    const int mH = bottom - topOff;

    QVector<GDALDatasetH> srcDS(nIW), cohDS(nIW), alignDS(nIW);
    QVector<GDALRasterBandH> srcBand(nIW), cohBand(nIW), alignBand(nIW);
    QVector<int> srcW(nIW);
    QVector<int> srcOff(nIW);
    for (int i = 0; i < nIW; ++i) {
        srcDS[i] = GDALOpen(iwFiles[i].toUtf8().constData(), GA_ReadOnly);
        if (!srcDS[i]) { qWarning() << "[IWMerge] open fail:" << iwFiles[i]; return false; }
        srcBand[i] = GDALGetRasterBand(srcDS[i], 1);
        srcW[i] = iwMetas[i].width;
        srcOff[i] = iwMetas[i].azimuthOffset;
        if (!cohFiles.isEmpty() && i < cohFiles.size() && !cohFiles[i].isEmpty()) {
            cohDS[i] = GDALOpen(cohFiles[i].toUtf8().constData(), GA_ReadOnly);
            if (cohDS[i]) cohBand[i] = GDALGetRasterBand(cohDS[i], 1);
        }
        if (i < alignPhFiles.size() && !alignPhFiles[i].isEmpty()) {
            alignDS[i] = GDALOpen(alignPhFiles[i].toUtf8().constData(), GA_ReadOnly);
            if (alignDS[i]) alignBand[i] = GDALGetRasterBand(alignDS[i], 1);
        }
    }

    // ── 相位一致性对齐: 逐 seam 估计 (鲁棒常数+线性), 链式累积 ──
    // corr_i(col) = a_i + b_i·col (i=0 恒 0)
    QVector<double> aCorr(nIW, 0.0), bCorr(nIW, 0.0);
    for (int i = 0; i < nIW - 1; ++i) {
        AlignResult est = estimateSeamAlignment(
            alignBand[i], cohBand[i], iwMetas[i].width, iwMetas[i].height, iwMetas[i].azimuthOffset,
            alignBand[i + 1], cohBand[i + 1], iwMetas[i + 1].width, iwMetas[i + 1].height, iwMetas[i + 1].azimuthOffset,
            overlapTrim);
        const double colBase = iwMetas[i].width - overlapTrim;   // A 侧重叠起始列
        if (est.ok) {
            // 链式: corr_{i+1}(c) = corr_i(colBase + c) + Δφ(c)
            aCorr[i + 1] = aCorr[i] + bCorr[i] * colBase + est.a;
            bCorr[i + 1] = bCorr[i] + est.b;
        } else {
            aCorr[i + 1] = aCorr[i] + bCorr[i] * colBase;   // 延续上一多项式
            bCorr[i + 1] = bCorr[i];
        }
        qDebug().nospace() << "[IWMerge] seam " << i << "->" << (i + 1)
            << " align a=" << (aCorr[i + 1] * 180.0 / M_PI) << "deg"
            << " b=" << (bCorr[i + 1] * 180.0 / M_PI) << "deg/col"
            << " rawB=" << (est.rawB * 180.0 / M_PI) << "deg/col"
            << " meanCoh=" << est.meanCoh << " R2=" << est.r2
            << (est.ok ? "" : " (unreliable)");
    }

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dstDS = GDALCreate(drv, outputPath.toUtf8().constData(),
        mW, mH, 1, dataType, nullptr);
    if (!dstDS) {
        for (int i = 0; i < nIW; ++i) {
            GDALClose(srcDS[i]);
            if (cohDS[i]) GDALClose(cohDS[i]);
        }
        return false;
    }
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(dstDS, gt);
    GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);

    std::vector<T> outBuf(mW);
    std::vector<T> iwBuf;

    for (int row = topOff; row < bottom; ++row) {
        std::fill(outBuf.begin(), outBuf.end(), T{});
        int dstCol = 0;
        for (int iw = 0; iw < nIW; ++iw) {
            const int srcRow = row - srcOff[iw];
            // 重叠区两侧都裁剪: 子条带边缘 = 天线滚降带 (幅度低 → 黑带)
            const int colOff = (iw > 0 ? overlapTrim : 0);
            const int copyW = srcW[iw] - colOff - (iw < nIW - 1 ? overlapTrim : 0);
            if (srcRow >= 0 && srcRow < iwMetas[iw].height
                && readRow<T>(srcBand[iw], srcRow, srcW[iw], iwBuf)) {
                for (int c = 0; c < copyW; ++c) {
                    // 校正多项式在源列坐标上评估 (源列 = colOff + c)
                    const double corr = aCorr[iw] + bCorr[iw] * (colOff + c);
                    outBuf[dstCol + c] = rotatePixel<T>(iwBuf[colOff + c], -corr);
                }
            }
            dstCol += copyW;
        }
        writeRow<T>(dstBand, row - topOff, mW, outBuf);
        if ((row - topOff) % 100 == 0) qDebug() << "[IWMerge] row" << (row - topOff) << "/" << mH;
    }

    GDALClose(dstDS);
    for (int i = 0; i < nIW; ++i) {
        GDALClose(srcDS[i]);
        if (cohDS[i]) GDALClose(cohDS[i]);
        if (alignDS[i]) GDALClose(alignDS[i]);
    }
    qDebug() << "[IWMerge] aligned concat" << mW << "x" << mH << "->" << outputPath;
    return true;
}

// 相位旋转: float = 相位减法回绕; complex = 复数旋转
template<>
float rotatePixel<float>(float v, double ph)
{
    double x = v - ph;
    x = std::fmod(x + M_PI, 2.0 * M_PI);
    if (x < 0) x += 2.0 * M_PI;
    return static_cast<float>(x - M_PI);
}

template<>
std::complex<float> rotatePixel<std::complex<float>>(std::complex<float> v, double ph)
{
    const float c = std::cos(static_cast<float>(ph));
    const float s = std::sin(static_cast<float>(ph));
    return std::complex<float>(v.real() * c - v.imag() * s, v.imag() * c + v.real() * s);
}

// ── 对外接口 ──

bool mergePhase(const QVector<QString>& iwFiles, const QVector<QString>& cohFiles,
    const QVector<IwMeta>& iwMetas, const QString& outputPath, bool alignEnabled)
{
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50;
    if (!alignEnabled)
        return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32, trim);
    return mergeAlignedT<float>(iwFiles, cohFiles, iwFiles, iwMetas, outputPath, GDT_Float32, trim);
}

bool mergeCoherence(const QVector<QString>& iwFiles,
    const QVector<IwMeta>& iwMetas, const QString& outputPath)
{
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50;
    return mergeT<float>(iwFiles, iwMetas, outputPath, GDT_Float32, trim);
}

bool mergeComplex(const QVector<QString>& iwFiles, const QVector<QString>& cohFiles,
    const QVector<QString>& alignPhaseFiles,
    const QVector<IwMeta>& iwMetas, const QString& outputPath, bool alignEnabled)
{
    int trim = computeOverlapTrim(iwMetas);
    if (trim <= 0) trim = 50;
    if (!alignEnabled)
        return mergeT<std::complex<float>>(iwFiles, iwMetas, outputPath, GDT_CFloat32, trim);
    return mergeAlignedT<std::complex<float>>(iwFiles, cohFiles, alignPhaseFiles,
        iwMetas, outputPath, GDT_CFloat32, trim);
}

// ═══════════════════════════════════════════════════════════
//  缝方位残余偏移估计 (数据驱动, 输出行单位)
//  重叠区(同地面)幅度纹理列平均剖面 → Pearson 相关 → 峰值行偏移
//  2026-08-15: merge 方位偏移用首 burst 时间差计算的固定值有残余误差,
//  拼接处纹理纵向错位。相位相关在低相干下无分辨力 (峰平坦、总在边界),
//  改用幅度纹理 — 场景结构沿方位持久, 相关峰尖锐。
// ═══════════════════════════════════════════════════════════
bool estimateSeamAzimuthShift(const QString& phA, const QString& phB,
                              int wA, int hA, int wB, int hB,
                              int overlapCols, int maxShiftRows,
                              int* shiftRows, double* peakValue)
{
    if (shiftRows) *shiftRows = 0;
    if (peakValue) *peakValue = 0.0;
    if (overlapCols <= 0 || hA <= 0 || hB <= 0) return false;

    GDALDatasetH dA = GDALOpen(phA.toUtf8().constData(), GA_ReadOnly);
    GDALDatasetH dB = GDALOpen(phB.toUtf8().constData(), GA_ReadOnly);
    if (!dA || !dB) {
        if (dA) GDALClose(dA);
        if (dB) GDALClose(dB);
        return false;
    }
    GDALRasterBandH bA = GDALGetRasterBand(dA, 1);
    GDALRasterBandH bB = GDALGetRasterBand(dB, 1);
    const int ov = std::min(overlapCols, std::min(wA, wB));

    // 重叠列: A 的最后 ov 列, B 的最前 ov 列 (同地面)
    const int cA0 = wA - ov, cB0 = 0;

    // 列平均幅度剖面 (log 幅度纹理; 场景结构沿方位持久, 相关峰尖锐)
    std::vector<CFloat32> rowBuf(static_cast<size_t>(ov));
    std::vector<double> ampA(static_cast<size_t>(hA)), ampB(static_cast<size_t>(hB));
    for (int r = 0; r < hA; ++r) {
        GDALRasterIO(bA, GF_Read, cA0, r, ov, 1, rowBuf.data(), ov, 1, GDT_CFloat32, 0, 0);
        double s = 0;
        for (int c = 0; c < ov; ++c)
            s += std::sqrt(static_cast<double>(rowBuf[c].re) * rowBuf[c].re
                         + static_cast<double>(rowBuf[c].im) * rowBuf[c].im);
        ampA[r] = std::log10(std::max(1.0, s / ov));
    }
    for (int r = 0; r < hB; ++r) {
        GDALRasterIO(bB, GF_Read, cB0, r, ov, 1, rowBuf.data(), ov, 1, GDT_CFloat32, 0, 0);
        double s = 0;
        for (int c = 0; c < ov; ++c)
            s += std::sqrt(static_cast<double>(rowBuf[c].re) * rowBuf[c].re
                         + static_cast<double>(rowBuf[c].im) * rowBuf[c].im);
        ampB[r] = std::log10(std::max(1.0, s / ov));
    }
    GDALClose(dA); GDALClose(dB);

    // 去趋势: 减线性拟合 (天线方向图等低频趋势会在边界位移处产生假相关峰,
    // 2026-08-15 实测 VH seam 相关单调升到 ±12 边界的教训)
    auto detrend = [](std::vector<double>& a) {
        const int n = static_cast<int>(a.size());
        if (n < 8) return;
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int r = 0; r < n; ++r) {
            sx += r; sy += a[r]; sxx += static_cast<double>(r) * r;
            sxy += static_cast<double>(r) * a[r];
        }
        const double b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        const double c = (sy - b * sx) / n;
        for (int r = 0; r < n; ++r) a[r] -= (c + b * r);
    };
    detrend(ampA);
    detrend(ampB);

    // Pearson 互相关: C[d] = Σ(aA−ā)(aB−ā') / (n·σA·σB)
    double mA = 0, mB = 0;
    for (double v : ampA) mA += v;
    for (double v : ampB) mB += v;
    mA /= hA; mB /= hB;
    double vA = 0, vB = 0;
    for (double v : ampA) vA += (v - mA) * (v - mA);
    for (double v : ampB) vB += (v - mB) * (v - mB);
    vA = std::sqrt(vA / hA); vB = std::sqrt(vB / hB);
    if (vA < 1e-9 || vB < 1e-9) return false;

    double bestV = -2.0; int bestD = 0;
    double secondV = -2.0; int secondD = 0;
    for (int d = -maxShiftRows; d <= maxShiftRows; ++d) {
        int rA0 = std::max(0, -d), rB0 = std::max(0, d);
        const int n = std::min(hA - rA0, hB - rB0);
        if (n < 64) continue;
        double s = 0;
        for (int k = 0; k < n; ++k)
            s += (ampA[rA0 + k] - mA) * (ampB[rB0 + k] - mB);
        const double v = s / (n * vA * vB);
        if (v > bestV) { secondV = bestV; secondD = bestD; bestV = v; bestD = d; }
        else if (v > secondV) { secondV = v; secondD = d; }
    }
    if (shiftRows) *shiftRows = bestD;
    if (peakValue) *peakValue = bestV;
    qDebug() << "[IWMerge] seamShift scan(amp): best d=" << bestD << "(corr="
             << QString::number(bestV, 'f', 3) << ") second d=" << secondD
             << "(corr=" << QString::number(secondV, 'f', 3) << ") range=±"
             << maxShiftRows;
    return true;
}

} // namespace IWMerger
