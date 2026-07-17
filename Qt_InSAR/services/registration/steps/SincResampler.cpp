#include "SincResampler.h"
#include "../PipelineContext.h"
#include "algorithms/SincInterpolator.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalSlcWriter.h"
#include <QDebug>
#include <QApplication>
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
    writer.setGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
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

bool SincResampler::resampleTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    double prf    = p.masterPrf;
    double kt     = ctx.data.slaveAzimuthFmRate;
    bool doDeramp = (std::abs(kt) > 1e-6) && (prf > 0);

    if (ctx.burstResults.size() < N) {
        ctx.errorMessage = "SincResampler: burstResults not populated"; return false;
    }

    // ── Step A: 计算每对burst间的overlap和cut line ──
    QVector<int> discardBottom(N, 0), discardTop(N, 0);
    const auto& mT = ctx.data.masterBurstTimes;

    if (mT.size() >= N) {
        for (int b = 0; b < N - 1; ++b) {
            double dt = mT[b].msecsTo(mT[b+1]) / 1000.0;   // 秒
            double burstDur = L / prf;
            double overlapTime = burstDur - dt;
            if (overlapTime < 0) overlapTime = 0;
            int overlapLines = (int)(overlapTime * prf + 0.5);

            // cut在overlap的中点(azimuth时间), 每burst丢弃一半overlap
            int half = overlapLines / 2;
            discardBottom[b]   = half;
            discardTop[b + 1]  = half;
        }
    } else {
        // 无burst时间: 固定overlap=80行
        for (int b = 0; b < N - 1; ++b) {
            discardBottom[b]   = 40;
            discardTop[b + 1]  = 40;
        }
    }

    // ── Step B: 每burst保留范围 + 输出偏移 ──
    QVector<int> keepStart(N), keepEnd(N), outOffset(N);
    int outH = 0;
    for (int b = 0; b < N; ++b) {
        keepStart[b] = discardTop[b];
        keepEnd[b]   = L - 1 - discardBottom[b];
        outOffset[b] = outH;
        outH += (keepEnd[b] - keepStart[b] + 1);
    }

    qDebug() << QStringLiteral("[Step9] TOPSAR deburst %1x%2→%3x%4 %5burts deramp=%6")
        .arg(mW).arg(mH).arg(mW).arg(outH).arg(N)
        .arg(doDeramp ? "on" : "off");

    // ── Step C: 创建输出 ──
    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, outH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    writer.setGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    int step = std::max(1, outH / 100);

    // ── Step D: 逐burst逐行重采样 + deramp ──
    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2 rows %3-%4...")
            .arg(b+1).arg(N).arg(keepStart[b]).arg(keepEnd[b]);

        const auto& br = ctx.burstResults[b];
        int startRow = b * L;

        for (int r = keepStart[b]; r <= keepEnd[b]; ++r) {
            int gRowOut = outOffset[b] + (r - keepStart[b]);
            int gRowSrc = startRow + r;

            auto rc = computeRowCoords(gRowSrc, mW, mH, sH,
                br.rangePoly, br.aziPoly, readR);

            double eta = (doDeramp) ? (r - L/2.0) / prf : 0.0;
            double dp = (doDeramp) ? -M_PI * kt * eta * eta : 0.0;
            float dCos = (float)std::cos(dp);
            float dSin = (float)std::sin(dp);

            QVector<std::complex<float>> rowBuf(mW);
            if (rc.sYH <= 0) {
                rowBuf.fill({0, 0});
            } else {
                auto strip = ctx.slaveReader->readBandWindow(0, 0, rc.sY0, sW, rc.sYH);
                for (int c = 0; c < mW; ++c) {
                    auto val = interpFromStrip(strip, sW, rc.sYH,
                        rc.sx[c], rc.syFrac, useSinc, sincW, beta);
                    if (doDeramp && val != std::complex<float>{0, 0}) {
                        float re = val.real() * dCos - val.imag() * dSin;
                        float im = val.real() * dSin + val.imag() * dCos;
                        val = {re, im};
                    }
                    rowBuf[c] = val;
                }
            }
            writer.writeRow(gRowOut, rowBuf);
            if (gRowOut % step == 0) QApplication::processEvents();
        }
        QApplication::processEvents();
    }
    return true;
}

bool SincResampler::execute(PipelineContext& ctx) {
    if (ctx.isTopsar) return resampleTopsar(ctx);
    else return resampleNonTopsar(ctx);
}
