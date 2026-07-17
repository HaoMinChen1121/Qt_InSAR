#include "SincResampler.h"
#include "../PipelineContext.h"
#include "algorithms/SincInterpolator.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalSlcWriter.h"
#include <QDebug>
#include <QApplication>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include <gdal_priv.h>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool SincResampler::resampleNonTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    if (ctx.masterReader)
        writer.copyGeoreferencing(ctx.masterReader->datasetHandle(), QString());
    qDebug() << QStringLiteral("[Step9] non-TOPSAR resample %1x%2 method=%3").arg(mW).arg(mH).arg(p.resamplingMethod);

    QVector<std::complex<float>> rowBuf(mW);
    int step = std::max(1, mH / 100);
    for (int row = 0; row < mH; ++row) {
        if (mCancelled) return false;
        double aLoc = static_cast<double>(row) / mH;
        double rowOff = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1] * aLoc;
        int sRowBase = row + (int)rowOff;
        double syFrac = rowOff - (int)rowOff;

        int sY0 = sRowBase - readR; int sYH = readR * 2 + 1;
        if (sY0 < 0) { sYH += sY0; sY0 = 0; }
        if (sY0 + sYH > sH) sYH = sH - sY0;
        if (sYH <= 0) { rowBuf.fill({0, 0}); }
        else {
            auto sWin = ctx.slaveReader->readBandWindow(0, 0, sY0, sW, sYH);
            for (int c = 0; c < mW; ++c) {
                double rN = static_cast<double>(c) / mW;
                double colOff = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rN
                    + ctx.rangePoly.coeffs[2]*aLoc + ctx.rangePoly.coeffs[3]*rN*aLoc
                    + ctx.rangePoly.coeffs[4]*rN*rN + ctx.rangePoly.coeffs[5]*aLoc*aLoc;
                double sx = c + colOff, sy = syFrac;
                if (sx >= 0 && sx < sW - 1)
                    rowBuf[c] = useSinc ? sincInterp2D(sWin, sW, sYH, sx, sy, sincW, beta)
                                        : bilinearInterp2D(sWin, sW, sYH, sx, sy);
                else
                    rowBuf[c] = {0, 0};
            }
        }
        writer.writeRow(row, rowBuf);
        if (row % step == 0) QApplication::processEvents();
    }
    return true;
}

// ── 从内存条带内插单个像素 ──
static std::complex<float> interpFromStrip(const QVector<std::complex<float>>& strip,
    int sW, int sH, double sx, double sy,
    bool useSinc, int sincW, double beta)
{
    if (sx < 0 || sx >= sW - 1) return {0, 0};
    return useSinc ? sincInterp2D(strip, sW, sH, sx, sy, sincW, beta)
                   : bilinearInterp2D(strip, sW, sH, sx, sy);
}

// ── 计算一行的 slave 坐标 ──
struct RowCoords {
    QVector<double> sx;
    double syFrac;
    int sY0, sYH;
};

static RowCoords computeRowCoords(int gRow, int mW, int mH, int sH,
    const RangePolynomial& rP, const AzimuthPolynomial& aP, int readR)
{
    RowCoords rc;
    rc.sx.resize(mW);
    double aLoc = (double)gRow / mH;
    double rowOff = aP.coeffs[0] + aP.coeffs[1] * aLoc;
    int sRowBase = gRow + (int)rowOff;
    rc.syFrac = rowOff - (int)rowOff;

    rc.sY0 = sRowBase - readR; rc.sYH = readR * 2 + 1;
    if (rc.sY0 < 0) { rc.sYH += rc.sY0; rc.sY0 = 0; }
    if (rc.sY0 + rc.sYH > sH) rc.sYH = sH - rc.sY0;

    for (int c = 0; c < mW; ++c) {
        double rN = (double)c / mW;
        double colOff = rP.coeffs[0] + rP.coeffs[1]*rN + rP.coeffs[2]*aLoc
                      + rP.coeffs[3]*rN*aLoc + rP.coeffs[4]*rN*rN + rP.coeffs[5]*aLoc*aLoc;
        rc.sx[c] = c + colOff;
    }
    return rc;
}

// ── 并行重采样批次 ──
struct ResampleWorkItem {
    int gRowSrc, gRowOut;
    RangePolynomial rangePoly;
    AzimuthPolynomial aziPoly;
};

struct ResampleConfig {
    QString slavePath;
    int sW, sH, mW, mH, N, L, readR;
    double prf, kt;
    bool doDeramp, useFastSinc;
    int sincW; double beta;
    QVector<QVector<float>>* sincLUT;
};

// 直接从 GDAL 读 strip (避免 GDALOpenShared 多线程锁争用)
static QVector<std::complex<float>> readBandWindowMT(
    GDALDatasetH hDS, int band, int col0, int row0, int w, int h)
{
    if (!hDS) return {};
    if (col0 < 0) { w += col0; col0 = 0; }
    if (row0 < 0) { h += row0; row0 = 0; }
    int bw = GDALGetRasterXSize(hDS), bh = GDALGetRasterYSize(hDS);
    if (col0 + w > bw) w = bw - col0;
    if (row0 + h > bh) h = bh - row0;
    if (w <= 0 || h <= 0) return {};
    QVector<std::complex<float>> buf(w * h);
    GDALRasterIO(GDALGetRasterBand(hDS, band + 1), GF_Read,
        col0, row0, w, h, buf.data(), w, h, GDT_CFloat32, 0, 0);
    return buf;
}

static QVector<QPair<int, QVector<std::complex<float>>>> processResampleBatch(
    QVector<ResampleWorkItem> batch, ResampleConfig cfg)
{
    GDALDatasetH hDS = GDALOpen(cfg.slavePath.toUtf8().constData(), GA_ReadOnly);
    if (!hDS) return {};

    QVector<QPair<int, QVector<std::complex<float>>>> results;
    QVector<std::complex<float>> tempBuf;
    QVector<double> syBuf;

    for (const auto& w : batch) {
        auto rc = computeRowCoords(w.gRowSrc, cfg.mW, cfg.mH, cfg.sH,
            w.rangePoly, w.aziPoly, cfg.readR);

        QVector<std::complex<float>> rowBuf(cfg.mW);
        if (rc.sYH <= 0) {
            rowBuf.fill({0, 0});
        } else {
            auto strip = readBandWindowMT(hDS, 0, 0, rc.sY0, cfg.sW, rc.sYH);

            // Deramp slave strip BEFORE interpolation
            if (cfg.doDeramp) {
                for (int sr = 0; sr < rc.sYH; ++sr) {
                    int slaveRow = rc.sY0 + sr;
                    int sbIdx = qBound(0, slaveRow / cfg.L, cfg.N - 1);
                    double eta_S = (slaveRow - sbIdx * cfg.L - cfg.L/2.0) / cfg.prf;
                    double dp = -M_PI * cfg.kt * eta_S * eta_S;
                    float dCos = (float)std::cos(dp), dSin = (float)std::sin(dp);
                    int base = sr * cfg.sW, end = base + cfg.sW;
                    if (end > strip.size()) end = strip.size();
                    for (int idx = base; idx < end; ++idx) {
                        auto v = strip[idx];
                        float re = v.real() * dCos - v.imag() * dSin;
                        float im = v.real() * dSin + v.imag() * dCos;
                        strip[idx] = {re, im};
                    }
                }
            }

            // Interpolate
            if (cfg.useFastSinc) {
                double syVal = rc.syFrac + cfg.readR;
                syBuf.resize(cfg.mW);
                syBuf.fill(syVal);
                sincInterp1D_Horizontal(strip, cfg.sW, rc.sYH, rc.sx,
                    *cfg.sincLUT, cfg.sincW, tempBuf, cfg.mW);
                sincInterp1D_Vertical(tempBuf, rc.sYH, cfg.mW, syBuf,
                    *cfg.sincLUT, cfg.sincW, rowBuf.data());
            } else {
                for (int c = 0; c < cfg.mW; ++c) {
                    rowBuf[c] = interpFromStrip(strip, cfg.sW, rc.sYH,
                        rc.sx[c], rc.syFrac, true, cfg.sincW, cfg.beta);
                }
            }
        }
        results.append({w.gRowOut, std::move(rowBuf)});
    }
    GDALClose(hDS);
    return results;
}

bool SincResampler::resampleTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    double prf    = (ctx.data.masterAzimuthFrequency > 0)
                         ? ctx.data.masterAzimuthFrequency : p.masterPrf;
    double kt     = ctx.data.slaveAzimuthFmRate;
    bool doDeramp = (std::abs(kt) > 1e-6) && (prf > 0);

    // 预计算 Sinc 权重表 (加速: 查表替代 sinc()+Kaiser() 调用)
    QVector<QVector<float>> sincLUT;
    bool useFastSinc = useSinc;
    if (useFastSinc) {
        initSincLUT(sincW, beta, sincLUT);
        qDebug() << QStringLiteral("[Step9] Sinc LUT ready (%1 levels)")
            .arg(sincLUT.size());
    }

    qDebug() << QStringLiteral("[Step9] band=%1 usingPrf=%2 Hz")
        .arg(ctx.masterBand->subSwath).arg(prf, 0, 'f', 2);

    if (ctx.burstResults.size() < N) {
        ctx.errorMessage = "SincResampler: burstResults not populated"; return false;
    }

    qDebug() << QStringLiteral("[Step9] TOPSAR resample %1x%2 %3bursts (no deburst) deramp=%4")
        .arg(mW).arg(mH).arg(N).arg(doDeramp ? "on" : "off");

    // ── 创建输出: 全尺寸,保留burst结构,deburst在干涉图模块做 ──
    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    if (ctx.masterReader)
        writer.copyGeoreferencing(ctx.masterReader->datasetHandle(), QString());

    int step = std::max(1, mH / 100);
    int nThreads = qBound(1, QThread::idealThreadCount(), 4);

    ResampleConfig rcfg;
    rcfg.slavePath = ctx.slaveBand->rasterPath;
    rcfg.sW = sW; rcfg.sH = sH; rcfg.mW = mW; rcfg.mH = mH;
    rcfg.N = N; rcfg.L = L; rcfg.readR = readR;
    rcfg.prf = prf; rcfg.kt = kt;
    rcfg.doDeramp = doDeramp; rcfg.useFastSinc = useFastSinc;
    rcfg.sincW = sincW; rcfg.beta = beta;
    rcfg.sincLUT = &sincLUT;

    // ── 逐burst并行重采样 (全burst,不裁overlap) ──
    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2...").arg(b+1).arg(N);

        const auto& br = ctx.burstResults[b];
        int burstRow0 = b * L;

        // 全burst行 (不含deburst裁切)
        QVector<ResampleWorkItem> items;
        for (int r = 0; r < L; ++r)
            items.append({burstRow0 + r, burstRow0 + r,
                          br.rangePoly, br.aziPoly});

        // 按线程数分批
        int batchSz = qMax(1, (items.size() + nThreads - 1) / nThreads);
        QList<QVector<ResampleWorkItem>> batches;
        for (int i = 0; i < items.size(); i += batchSz) {
            QVector<ResampleWorkItem> batch;
            for (int j = i; j < qMin(i + batchSz, items.size()); ++j)
                batch.append(items[j]);
            batches.append(batch);
        }

        // 并行处理
        QList<QFuture<QVector<QPair<int, QVector<std::complex<float>>>>>> futures;
        for (int i = 0; i < batches.size(); ++i)
            futures.append(QtConcurrent::run(processResampleBatch, batches[i], rcfg));

        // 收集结果并按行号排序写入
        QVector<QPair<int, QVector<std::complex<float>>>> allRows;
        for (auto& f : futures)
            allRows.append(f.result());
        std::sort(allRows.begin(), allRows.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& row : allRows) {
            writer.writeRow(row.first, row.second);
            if (row.first % step == 0) QApplication::processEvents();
        }
        QApplication::processEvents();
    }
    return true;
}

bool SincResampler::execute(PipelineContext& ctx) {
    if (ctx.isTopsar) return resampleTopsar(ctx);
    else return resampleNonTopsar(ctx);
}
