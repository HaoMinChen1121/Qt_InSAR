#include "SincResampler.h"
#include "../PipelineContext.h"
#include "algorithms/SincInterpolator.h"
#include "algorithms/ComplexSoA.h"
#include "algorithms/DerampCore.h"
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

// ── 从全burst缓冲区提取条带 ──
// sY0/sYH 是全局 slave 行坐标, burstRow0 是当前 burst 在 slave 中的起始行
static QVector<std::complex<float>> extractStrip(
    const QVector<std::complex<float>>& fullBurst,
    int sW, int burstH, int burstRow0, int sY0, int sYH)
{
    if (sYH <= 0) return {};
    int localY0 = qMax(sY0, burstRow0) - burstRow0;
    int localY1 = qMin(sY0 + sYH, burstRow0 + burstH) - burstRow0;
    int h = localY1 - localY0;
    if (h <= 0) return {};
    QVector<std::complex<float>> strip(sW * h);
    memcpy(strip.data(), fullBurst.data() + localY0 * sW,
           sW * h * sizeof(std::complex<float>));
    return strip;
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
    const QVector<std::complex<float>>* fullBurst = nullptr;  // 全burst内存缓冲区(未deramp)
    int sW, burstH, burstRow0, sH, mW, mH, readR;
    bool useFastSinc;
    int sincW; double beta;
    QVector<QVector<float>>* sincLUT;
    // deramp 参数 (worker 内对 strip 做 deramp)
    bool doDeramp;
    double prf, kt;
    int burstIdx, L;  // burstIdx = 当前burst索引 (0-based), L = linesPerBurst
};

static QVector<QPair<int, QVector<std::complex<float>>>> processResampleBatch(
    QVector<ResampleWorkItem> batch, ResampleConfig cfg)
{
    QVector<QPair<int, QVector<std::complex<float>>>> results;
    QVector<std::complex<float>> tempBuf;
    QVector<double> syBuf;

    for (const auto& w : batch) {
        auto rc = computeRowCoords(w.gRowSrc, cfg.mW, cfg.mH, cfg.sH,
            w.rangePoly, w.aziPoly, cfg.readR);

        QVector<std::complex<float>> rowBuf(cfg.mW);
        int actualStripH = rc.sYH;
        if (actualStripH <= 0) {
            rowBuf.fill({0, 0});
        } else {
            auto strip = extractStrip(*cfg.fullBurst, cfg.sW,
                cfg.burstH, cfg.burstRow0, rc.sY0, rc.sYH);
            // extractStrip 可能在边界裁剪，用实际 strip 高度替代 rc.sYH
            actualStripH = strip.size() / cfg.sW;
            if (actualStripH <= 0) {
                rowBuf.fill({0, 0});
                results.append({w.gRowOut, std::move(rowBuf)});
                continue;
            }

            // 对提取的 strip 做 deramp
            if (cfg.doDeramp) {
                for (int sr = 0; sr < actualStripH; ++sr) {
                    int slaveRow = rc.sY0 + sr;
                    int sbIdx = qBound(0, slaveRow / cfg.L, cfg.burstIdx + 1);
                    double eta_S = (slaveRow - sbIdx * cfg.L - cfg.L / 2.0) / cfg.prf;
                    double dp = -M_PI * cfg.kt * eta_S * eta_S;
                    float dCos = (float)std::cos(dp), dSin = (float)std::sin(dp);
                    int base = sr * cfg.sW, end = base + cfg.sW;
                    for (int idx = base; idx < end; ++idx) {
                        auto v = strip[idx];
                        strip[idx] = {
                            v.real() * dCos - v.imag() * dSin,
                            v.real() * dSin + v.imag() * dCos};
                    }
                }
            }

            if (cfg.useFastSinc) {
                double syFracInStrip = rc.syFrac + (rc.sY0 - qMax(rc.sY0, cfg.burstRow0));
                syBuf.resize(cfg.mW);
                syBuf.fill(syFracInStrip + cfg.readR);
                sincInterp1D_Horizontal(strip, cfg.sW, actualStripH, rc.sx,
                    *cfg.sincLUT, cfg.sincW, tempBuf, cfg.mW);
                sincInterp1D_Vertical(tempBuf, actualStripH, cfg.mW, syBuf,
                    *cfg.sincLUT, cfg.sincW, rowBuf.data());
            } else {
                for (int c = 0; c < cfg.mW; ++c) {
                    rowBuf[c] = interpFromStrip(strip, cfg.sW, actualStripH,
                        rc.sx[c], rc.syFrac, true, cfg.sincW, cfg.beta);
                }
            }
        }
        results.append({w.gRowOut, std::move(rowBuf)});
    }
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

    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    if (ctx.masterReader)
        writer.copyGeoreferencing(ctx.masterReader->datasetHandle(), QString());

    int step = std::max(1, mH / 100);
    int nThreads = qBound(1, QThread::idealThreadCount(), 12);

    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2 reading full burst...").arg(b+1).arg(N);

        const auto& br = ctx.burstResults[b];
        int burstRow0 = b * L;

        // ── 使用已打开的 slaveReader 一次读入整个 burst ──
        QVector<std::complex<float>> fullBurst =
            ctx.slaveReader->readBandWindow(0, 0, burstRow0, sW, L);
        if (fullBurst.isEmpty()) {
            ctx.errorMessage = QStringLiteral("SincResampler: readBandWindow empty");
            return false;
        }

        // ── Deramp: 整个burst一次性完成 (修复原per-strip的33x冗余) ──
        if (doDeramp) {
            sar::ComplexSoA soa;
            soa.fromAos(fullBurst.constData(), sW * L);
            sar::applyDeramp_SoA(soa, sW, L, burstRow0, b, prf, kt);
            soa.toAos(fullBurst.data(), sW * L);
        }

        // ── 构建工作项 + 分批 ──
        QVector<ResampleWorkItem> items;
        items.reserve(L);
        for (int r = 0; r < L; ++r)
            items.append({burstRow0 + r, burstRow0 + r,
                          br.rangePoly, br.aziPoly});

        int batchSz = qMax(1, (items.size() + nThreads * 4 - 1) / (nThreads * 4));
        QList<QVector<ResampleWorkItem>> batches;
        for (int i = 0; i < items.size(); i += batchSz) {
            QVector<ResampleWorkItem> batch;
            for (int j = i; j < qMin(i + batchSz, items.size()); ++j)
                batch.append(items[j]);
            batches.append(batch);
        }

        // ── 插值 (所有线程共享 fullBurst 只读内存, deramp在worker内做) ──
        ResampleConfig rcfg;
        rcfg.fullBurst   = &fullBurst;
        rcfg.sW = sW; rcfg.burstH = L; rcfg.burstRow0 = burstRow0;
        rcfg.sH = sH;
        rcfg.mW = mW; rcfg.mH = mH;
        rcfg.readR = readR;
        rcfg.useFastSinc = useFastSinc;
        rcfg.sincW = sincW; rcfg.beta = beta;
        rcfg.sincLUT = &sincLUT;
        rcfg.doDeramp = false;   // 已在burst级预deramp
        rcfg.prf = prf; rcfg.kt = kt;
        rcfg.burstIdx = b; rcfg.L = L;
        qDebug() << "[Step9] config ready, processing batches serially...";

        // ── 并行处理各批次 ──
        QList<QFuture<QVector<QPair<int, QVector<std::complex<float>>>>>> futures;
        for (int i = 0; i < batches.size(); ++i)
            futures.append(QtConcurrent::run(processResampleBatch, batches[i], rcfg));

        QVector<QPair<int, QVector<std::complex<float>>>> allRows;
        for (auto& f : futures)
            allRows += f.result();
        qDebug() << "[Step9] all batches done, sorting...";
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
