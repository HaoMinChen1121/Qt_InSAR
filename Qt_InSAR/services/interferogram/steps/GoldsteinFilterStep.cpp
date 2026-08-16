#include "GoldsteinFilterStep.h"
#include "../PipelineContext.h"
#include "algorithms/GoldsteinFilter.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

// Step 4: Goldstein 自适应滤波 (diff 之后、coh 估计之前, 对齐 GAMMA adf 链位置)
//   输入: 服务设置的 ctx.filteredIfgPath 为空时取 diff, 否则取 flat 产品
//   输出: diff/S1_POL_diff_filt.tif (+ _phase.tif), 供 CoherenceEstimator 使用
bool GoldsteinFilterStep::execute(IfgPipelineContext& ctx)
{
    const QString inPath = ctx.filteredIfgPath.isEmpty()
        ? ctx.diffOutputBase + "_diff.tif"
        : ctx.filteredIfgPath;
    if (!QFileInfo::exists(inPath)) {
        ctx.errorMessage = QStringLiteral("GoldsteinFilterStep: input missing %1").arg(inPath);
        return false;
    }

    GdalSlcReader reader;
    if (!reader.open(inPath)) {
        ctx.errorMessage = QStringLiteral("GoldsteinFilterStep: cannot open %1").arg(inPath);
        return false;
    }
    const int w = reader.width(), h = reader.height();

    // 读入整幅复数干涉图 (合并产品 ~92MB, 可接受)
    QVector<std::complex<float>> ifg(w * h);
    const int tileRows = 256;
    for (int r0 = 0; r0 < h; r0 += tileRows) {
        if (mCancelled || ctx.cancelled) { reader.close(); return false; }
        const int n = std::min(tileRows, h - r0);
        auto rows = reader.readBandWindow(0, 0, r0, w, n);
        for (int i = 0; i < n * w && r0 * w + i < ifg.size(); ++i)
            ifg[r0 * w + i] = rows.size() > i ? rows[i] : std::complex<float>(0, 0);
    }
    reader.close();

    qDebug() << "[Goldstein] filtering" << w << "x" << h;
    auto filtered = sar::goldsteinFilter(ifg.data(), w, h,
                                         0.5,   // alpha (ASF 用 0.6)
                                         32,    // patch
                                         3,     // smoothWin
                                         4096); // tile
    if (filtered.size() != ifg.size()) {
        ctx.errorMessage = QStringLiteral("GoldsteinFilterStep: filter returned empty");
        return false;
    }

    const QString outBase = ctx.diffOutputBase;
    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const QString filtPath = outBase + "_diff_filt.tif";
    const QString phPath = outBase + "_diff_filt_phase.tif";
    GDALDatasetH hF = GDALCreate(drv, filtPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hP = GDALCreate(drv, phPath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hF || !hP) {
        ctx.errorMessage = "GoldsteinFilterStep: cannot create output";
        return false;
    }
    GDALSetGeoTransform(hF, gt); GDALSetGeoTransform(hP, gt);

    QVector<float> phRow(w);
    for (int r = 0; r < h; ++r) {
        if (mCancelled || ctx.cancelled) { GDALClose(hF); GDALClose(hP); return false; }
        const auto* row = filtered.data() + static_cast<size_t>(r) * w;
        for (int c = 0; c < w; ++c)
            phRow[c] = std::atan2(row[c].imag(), row[c].real());
        // CFloat32 与 std::complex<float> 内存布局一致
        GDALRasterIO(GDALGetRasterBand(hF,1), GF_Write, 0, r, w, 1,
                     reinterpret_cast<CFloat32*>(filtered.data() + static_cast<size_t>(r) * w),
                     w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hP,1), GF_Write, 0, r, w, 1, phRow.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hF); GDALClose(hP);

    ctx.filteredIfgPath = filtPath;
    qDebug() << "[Goldstein] SUCCESS ->" << filtPath;
    return true;
}
