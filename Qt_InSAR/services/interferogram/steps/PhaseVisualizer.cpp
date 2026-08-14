#include "PhaseVisualizer.h"
#include "../PipelineContext.h"
#include "GeomTable.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <algorithm>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

// 每子条带段的幅度分位数 (避免跨子条带亮度阶跃 — 黑线成因)
struct SegStretch {
    int startCol = 0;
    int width = 0;
    double logP2 = 0, logP98 = 0, logSpan = 1.0;
    std::vector<double> samples;
};

// HSV → RGB (h,s,v ∈ [0,1])
inline void hsvToRgb(double h, double s, double v, unsigned char* rgb)
{
    if (s <= 0.0) {
        const unsigned char g = static_cast<unsigned char>(v * 255.0 + 0.5);
        rgb[0] = rgb[1] = rgb[2] = g;
        return;
    }
    h = std::fmod(h, 1.0);
    if (h < 0) h += 1.0;
    const double hh = h * 6.0;
    const int sec = static_cast<int>(hh);
    const double f = hh - sec;
    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));
    double r = 0, g = 0, b = 0;
    switch (sec % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = static_cast<unsigned char>(r * 255.0 + 0.5);
    rgb[1] = static_cast<unsigned char>(g * 255.0 + 0.5);
    rgb[2] = static_cast<unsigned char>(b * 255.0 + 0.5);
}

// 单组渲染: phase + ifg(幅度) + coh → RGB
// geom: 合并产品几何表 — 幅度分位数按子条带分段计算
// (全局拉伸会在子条带边界产生亮度阶跃 → 垂直黑线)
bool renderOne(const QString& phasePath, const QString& ifgPath,
               const QString& cohPath, const QString& outPath,
               const GeomTable* geom)
{
    GDALDatasetH hPh = GDALOpen(phasePath.toUtf8().constData(), GA_ReadOnly);
    GDALDatasetH hIfg = GDALOpen(ifgPath.toUtf8().constData(), GA_ReadOnly);
    if (!hPh || !hIfg) {
        if (hPh) GDALClose(hPh);
        if (hIfg) GDALClose(hIfg);
        qWarning() << "[Vis] cannot open input" << phasePath;
        return false;
    }
    GDALDatasetH hCoh = nullptr;
    if (!cohPath.isEmpty())
        hCoh = GDALOpen(cohPath.toUtf8().constData(), GA_ReadOnly);

    GDALRasterBandH bPh = GDALGetRasterBand(hPh, 1);
    GDALRasterBandH bIfg = GDALGetRasterBand(hIfg, 1);
    GDALRasterBandH bCoh = hCoh ? GDALGetRasterBand(hCoh, 1) : nullptr;
    const int w = GDALGetRasterXSize(hPh);
    const int h = GDALGetRasterYSize(hPh);

    // ── 幅度 2%/98% 分位数 (采样统计, 按子条带分段) ──
    QVector<SegStretch> segs;
    if (geom && !geom->swaths.isEmpty()) {
        for (const auto& s : geom->swaths) {
            SegStretch st;
            st.startCol = s.startCol;
            st.width = s.width;
            segs.append(st);
        }
    }
    if (segs.isEmpty()) {
        SegStretch st;
        st.startCol = 0;
        st.width = w;
        segs.append(st);
    }

    {
        const int rowStep = std::max(1, h / 200);
        const int colStep = std::max(1, w / 1000);
        std::vector<std::complex<float>> buf(static_cast<size_t>(w));
        for (int r = 0; r < h; r += rowStep) {
            if (GDALRasterIO(bIfg, GF_Read, 0, r, w, 1,
                    reinterpret_cast<CFloat32*>(buf.data()), w, 1, GDT_CFloat32, 0, 0) != CE_None)
                continue;
            for (int c = 0; c < w; c += colStep) {
                const auto& v = buf[c];
                const double amp = std::sqrt(v.real() * v.real() + v.imag() * v.imag());
                if (amp <= 0) continue;
                for (auto& st : segs) {
                    if (c >= st.startCol && c < st.startCol + st.width) {
                        st.samples.push_back(std::log10(amp));
                        break;
                    }
                }
            }
        }
    }
    int validSegs = 0;
    for (auto& st : segs) {
        if (st.samples.size() < 100) continue;
        std::sort(st.samples.begin(), st.samples.end());
        st.logP2 = st.samples[static_cast<size_t>(st.samples.size() * 0.02)];
        st.logP98 = st.samples[static_cast<size_t>(st.samples.size() * 0.98)];
        st.logSpan = std::max(1e-6, st.logP98 - st.logP2);
        ++validSegs;
    }
    if (validSegs == 0) {
        GDALClose(hPh); GDALClose(hIfg);
        if (hCoh) GDALClose(hCoh);
        qWarning() << "[Vis] too few amplitude samples";
        return false;
    }

    // ── 输出 3-band Byte ──
    QDir().mkpath(QFileInfo(outPath).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dst = GDALCreate(drv, outPath.toUtf8().constData(), w, h, 3, GDT_Byte, nullptr);
    if (!dst) {
        GDALClose(hPh); GDALClose(hIfg);
        if (hCoh) GDALClose(hCoh);
        return false;
    }
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(dst, gt);

    std::vector<float> phRow(static_cast<size_t>(w));
    std::vector<float> cohRow(static_cast<size_t>(w));
    std::vector<std::complex<float>> ifgRow(static_cast<size_t>(w));
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * 3);

    for (int r = 0; r < h; ++r) {
        GDALRasterIO(bPh, GF_Read, 0, r, w, 1, phRow.data(), w, 1, GDT_Float32, 0, 0);
        GDALRasterIO(bIfg, GF_Read, 0, r, w, 1,
            reinterpret_cast<CFloat32*>(ifgRow.data()), w, 1, GDT_CFloat32, 0, 0);
        if (bCoh)
            GDALRasterIO(bCoh, GF_Read, 0, r, w, 1, cohRow.data(), w, 1, GDT_Float32, 0, 0);
        for (int c = 0; c < w; ++c) {
            const double ph = phRow[c];
            const auto& v = ifgRow[c];
            const double amp = std::sqrt(v.real() * v.real() + v.imag() * v.imag());
            // H = (phase+π)/2π
            const double H = (ph + M_PI) / (2.0 * M_PI);
            // S = clip(0.2 + 0.8·coh) (无 coh 输入时 S=1)
            const double S = bCoh
                ? std::clamp(0.2 + 0.8 * static_cast<double>(cohRow[c]), 0.0, 1.0) : 1.0;
            // V = log 幅度分位数拉伸 (本子条带分段)
            double V = 0.0;
            if (amp > 0) {
                for (const auto& st : segs) {
                    if (c >= st.startCol && c < st.startCol + st.width) {
                        V = std::clamp((std::log10(amp) - st.logP2) / st.logSpan, 0.0, 1.0);
                        break;
                    }
                }
            }
            hsvToRgb(H, S, V, rgb.data() + static_cast<size_t>(c) * 3);
        }
        for (int b = 0; b < 3; ++b) {
            // 转置提取单波段行
            std::vector<unsigned char> bandRow(static_cast<size_t>(w));
            for (int c = 0; c < w; ++c)
                bandRow[c] = rgb[static_cast<size_t>(c) * 3 + b];
            GDALRasterIO(GDALGetRasterBand(dst, b + 1), GF_Write, 0, r, w, 1,
                bandRow.data(), w, 1, GDT_Byte, 0, 0);
        }
        if (r % 500 == 0) qDebug() << "[Vis] row" << r << "/" << h;
    }

    GDALClose(dst);
    GDALClose(hPh); GDALClose(hIfg);
    if (hCoh) GDALClose(hCoh);
    qDebug() << "[Vis] SUCCESS" << outPath << "segments=" << validSegs;
    return true;
}

} // namespace

bool PhaseVisualizer::execute(IfgPipelineContext& ctx)
{
    bool anyOk = false;

    GeomTable geom;
    geom.load(ctx.geomTablePath);

    // 合并产品相位 → 彩色
    if (renderOne(ctx.mergeOutputBase + "_phase.tif",
                  ctx.mergeOutputBase + "_ifg.tif",
                  ctx.mergeOutputBase + "_coh.tif",
                  ctx.visualizationOutputBase + "_phase_color.tif",
                  &geom))
        anyOk = true;

    // 差分相位 → 彩色 (仅当差分已生成)
    const QString diffPh = ctx.diffOutputBase + "_diff_phase.tif";
    const QString diffIfg = ctx.diffOutputBase + "_diff.tif";
    if (QFileInfo::exists(diffPh) && QFileInfo::exists(diffIfg)) {
        if (renderOne(diffPh, diffIfg, QString(),
                      ctx.visualizationOutputBase + "_diff_color.tif",
                      &geom))
            anyOk = true;
    }

    return anyOk;
}
