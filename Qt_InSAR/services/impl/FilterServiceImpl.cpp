#include "FilterServiceImpl.h"

#include "domain/product/ProductManager.h"
#include "domain/SarComplexTypes.h"
#include "algorithms/GoldsteinFilter.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FilterServiceImpl::FilterServiceImpl(QObject* parent)
    : IFilterService(parent) {}

void FilterServiceImpl::setParams(const FilterParams& params) { mParams = params; }
FilterParams FilterServiceImpl::params() const { return mParams; }

void FilterServiceImpl::preview()
{
    // TODO: 512×512 子集快速预览 (Phase 5)
    emit previewReady(QString());
}

void FilterServiceImpl::execute()
{
    mRunning = true;
    GDALAllRegister();

    if (mParams.inputProductId.isEmpty() || !mProductManager) {
        emit errorOccurred(QStringLiteral("未指定输入干涉产品"));
        emit finished(false, QString());
        mRunning = false;
        return;
    }

    // Product 驱动: 经 ProductManager 解析输入 (不找文件路径)
    InterferogramProduct prod = mProductManager->interferogram(mParams.inputProductId);
    if (!prod.isValid()) {
        emit errorOccurred(QStringLiteral("无法解析干涉产品: %1").arg(mParams.inputProductId));
        emit finished(false, QString());
        mRunning = false;
        return;
    }
    // 标准链 (GAMMA adf 位置): 滤波作用于差分干涉图 (地形相位已去除)。
    // 无 diff 产品时回退原始干涉图 — 密集地形条纹下滤波会退化, 仅打警告
    QString ifgPath = prod.correctedIfg(mParams.polarization);
    if (ifgPath.isEmpty()) {
        ifgPath = prod.complexIfg(mParams.polarization);
        if (!ifgPath.isEmpty())
            emit progressChanged(0, QStringLiteral("警告: 产品无差分干涉图, 对含地形相位的原始干涉图滤波"));
    }
    if (ifgPath.isEmpty()) {
        emit errorOccurred(QStringLiteral("产品无 %1 极化复数干涉图").arg(mParams.polarization));
        emit finished(false, QString());
        mRunning = false;
        return;
    }

    // ── 读取复数干涉图 ──
    emit progressChanged(5, QStringLiteral("读取复数干涉图 (%1)...").arg(mParams.polarization));
    GDALDatasetH hIn = GDALOpen(ifgPath.toUtf8().constData(), GA_ReadOnly);
    if (!hIn) {
        emit errorOccurred(QStringLiteral("无法打开: %1").arg(ifgPath));
        emit finished(false, QString());
        mRunning = false;
        return;
    }
    const int w = GDALGetRasterXSize(hIn);
    const int h = GDALGetRasterYSize(hIn);
    std::vector<std::complex<float>> ifg(static_cast<size_t>(w) * h);
    GDALRasterIO(GDALGetRasterBand(hIn, 1), GF_Read, 0, 0, w, h,
                 reinterpret_cast<CFloat32*>(ifg.data()), w, h, GDT_CFloat32, 0, 0);
    GDALClose(hIn);

    // ── Goldstein 滤波 (复数进复数出) ──
    // FFT 分块 = goldsteinWindowSize (32, 与 GAMMA adf/SNAP 默认一致);
    // goldsteinPatchSize 为 legacy 参数 (SNAP 语义为重叠块, v1 未用)
    emit progressChanged(15, QStringLiteral("Goldstein 滤波 (%1, α=%2, 块=%3)...")
        .arg(mParams.polarization).arg(mParams.goldsteinAlpha).arg(mParams.goldsteinWindowSize));
    auto filtered = sar::goldsteinFilter(ifg.data(), w, h,
        mParams.goldsteinAlpha, mParams.goldsteinWindowSize, 3, 4096);
    emit progressChanged(90, QStringLiteral("写出滤波产品..."));

    // ── 输出: filter/S1_XX_ifg_filtered.tif (复数) + 相位 tif ──
    // outputDir 留空 = 干涉产品目录下 filter/ 子目录 (产品目录约定)
    const QString outDir = mParams.outputDir.isEmpty()
        ? QFileInfo(mParams.inputProductId).absolutePath() + "/filter"
        : mParams.outputDir;
    QDir().mkpath(outDir);
    const QString prefix = mParams.outputPrefix.isEmpty()
        ? QStringLiteral("ifg_filtered") : mParams.outputPrefix;
    const QString outBase = QStringLiteral("%1/S1_%2_%3").arg(outDir, mParams.polarization, prefix);

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALDatasetH hOut = GDALCreate(drv, (outBase + ".tif").toUtf8().constData(),
                                   w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, (outBase + "_phase.tif").toUtf8().constData(),
                                   w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) {
        if (hOut) GDALClose(hOut);
        if (hPh) GDALClose(hPh);
        emit errorOccurred(QStringLiteral("无法创建滤波输出"));
        emit finished(false, QString());
        mRunning = false;
        return;
    }
    GDALSetGeoTransform(hOut, gt);
    GDALSetGeoTransform(hPh, gt);

    std::vector<float> rowPhase(static_cast<size_t>(w));
    for (int r = 0; r < h; ++r) {
        const auto* src = filtered.data() + static_cast<size_t>(r) * w;
        for (int c = 0; c < w; ++c)
            rowPhase[c] = std::atan2(src[c].imag(), src[c].real());
        GDALRasterIO(GDALGetRasterBand(hOut, 1), GF_Write, 0, r, w, 1,
                     reinterpret_cast<CFloat32*>(const_cast<std::complex<float>*>(src)),
                     w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh, 1), GF_Write, 0, r, w, 1,
                     rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut);
    GDALClose(hPh);

    emit progressChanged(100, QStringLiteral("滤波完成"));
    emit finished(true, outBase + "_phase.tif");
    mRunning = false;
}

void FilterServiceImpl::cancel() { mRunning = false; }
bool FilterServiceImpl::isRunning() const { return mRunning; }
