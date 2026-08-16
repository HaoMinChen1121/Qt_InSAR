#include "CoherenceEstimator.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <complex>
#include <vector>

// Step 5: 相干性估计 (滤波后产品上, 5×5 多视网格滑窗)
//   γ = |Σ_w ifg| / Σ_w |ifg|  (含零填充像素的窗 → coh=0)
//   输出覆盖 merge/S1_POL_coh.tif — 与 ASF corr.tif 同口径可比
bool CoherenceEstimator::execute(IfgPipelineContext& ctx)
{
    const QString inPath = ctx.filteredIfgPath.isEmpty()
        ? ctx.diffOutputBase + "_diff.tif"
        : ctx.filteredIfgPath;
    if (!QFileInfo::exists(inPath)) {
        ctx.errorMessage = QStringLiteral("CoherenceEstimator: input missing %1").arg(inPath);
        return false;
    }

    GdalSlcReader reader;
    if (!reader.open(inPath)) {
        ctx.errorMessage = QStringLiteral("CoherenceEstimator: cannot open %1").arg(inPath);
        return false;
    }
    const int w = reader.width(), h = reader.height();

    const QString cohPath = ctx.mergeOutputBase + "_coh.tif";
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALDatasetH hCoh = GDALCreate(drv, cohPath.toUtf8().constData(), w, h, 1,
                                   GDT_Float32, nullptr);
    if (!hCoh) {
        ctx.errorMessage = "CoherenceEstimator: cannot create output";
        reader.close();
        return false;
    }
    GDALSetGeoTransform(hCoh, gt);

    // 滑窗和用 cumsum 角点差分 (O(1)/像素)
    const int win = 5;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<double> cRe(n), cIm(n), cMag(n), cVal(n);
    const int tileRows = 256;
    size_t idx = 0;
    for (int r0 = 0; r0 < h; r0 += tileRows) {
        if (mCancelled || ctx.cancelled) { GDALClose(hCoh); reader.close(); return false; }
        const int nRows = std::min(tileRows, h - r0);
        auto rows = reader.readBandWindow(0, 0, r0, w, nRows);
        for (int i = 0; i < nRows; ++i) {
            for (int c = 0; c < w; ++c, ++idx) {
                const auto v = rows.size() > i * w + c ? rows[i * w + c]
                                                       : std::complex<float>(0, 0);
                cRe[idx] = v.real();
                cIm[idx] = v.imag();
                cMag[idx] = std::sqrt(static_cast<double>(v.real()) * v.real()
                                      + static_cast<double>(v.imag()) * v.imag());
                cVal[idx] = cMag[idx] > 1e-6 ? 1.0 : 0.0;
            }
        }
    }
    reader.close();

    // 2D cumsum (行前缀 + 列前缀)
    auto cumsum2d = [w, h](std::vector<double>& a) {
        for (int r = 0; r < h; ++r) {
            double run = 0;
            size_t base = static_cast<size_t>(r) * w;
            for (int c = 0; c < w; ++c) {
                run += a[base + c];
                a[base + c] = (r > 0 ? a[base - w + c] : 0.0) + run;
            }
        }
    };
    cumsum2d(cRe);
    cumsum2d(cIm);
    cumsum2d(cMag);
    cumsum2d(cVal);

    QVector<float> rowCoh(w);
    for (int r = 0; r < h; ++r) {
        if (mCancelled || ctx.cancelled) { GDALClose(hCoh); return false; }
        const int r0 = r - win / 2, r1 = r + win / 2;
        for (int c = 0; c < w; ++c) {
            const int c0 = c - win / 2, c1 = c + win / 2;
            float coh = 0.0f;
            if (r0 >= 0 && c0 >= 0 && r1 < h && c1 < w) {
                auto corner = [w, h](const std::vector<double>& a, int rr, int cc) {
                    if (rr < 0 || cc < 0) return 0.0;
                    return a[static_cast<size_t>(rr) * w + cc];
                };
                const double sRe = corner(cRe, r1, c1) - corner(cRe, r0 - 1, c1)
                                 - corner(cRe, r1, c0 - 1) + corner(cRe, r0 - 1, c0 - 1);
                const double sIm = corner(cIm, r1, c1) - corner(cIm, r0 - 1, c1)
                                 - corner(cIm, r1, c0 - 1) + corner(cIm, r0 - 1, c0 - 1);
                const double sMag = corner(cMag, r1, c1) - corner(cMag, r0 - 1, c1)
                                  - corner(cMag, r1, c0 - 1) + corner(cMag, r0 - 1, c0 - 1);
                const double sVal = corner(cVal, r1, c1) - corner(cVal, r0 - 1, c1)
                                  - corner(cVal, r1, c0 - 1) + corner(cVal, r0 - 1, c0 - 1);
                // 零填充守卫: 有效像素不足的窗 → coh=0 (防退化 coh=1)
                if (sVal >= win * win * 0.9 && sMag > 1e-9)
                    coh = static_cast<float>(std::sqrt(sRe * sRe + sIm * sIm) / sMag);
            }
            rowCoh[c] = coh;
        }
        GDALRasterIO(GDALGetRasterBand(hCoh, 1), GF_Write, 0, r, w, 1,
                     rowCoh.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hCoh);

    ctx.outputBand.cohFile = QStringLiteral("merge/S1_%1_coh.tif")
        .arg(ctx.outputBand.polarization);
    qDebug() << "[CoherenceEst] SUCCESS (5x5 on filtered product) ->" << cohPath;
    return true;
}
