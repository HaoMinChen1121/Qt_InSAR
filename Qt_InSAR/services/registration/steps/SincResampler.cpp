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

// ── TOPSAR 逐行内插 + deramp + overlap线性融合 ──
static std::complex<float> interpSlave(GdalSlcReader* reader, int sW, int sH,
    const RangePolynomial& rP, const AzimuthPolynomial& aP,
    double col, double row, int mW, int mH,
    bool useSinc, int sincW, double beta, int readR)
{
    double rN = col / mW, aN = row / mH;
    double colOff = rP.coeffs[0] + rP.coeffs[1]*rN + rP.coeffs[2]*aN
                  + rP.coeffs[3]*rN*aN + rP.coeffs[4]*rN*rN + rP.coeffs[5]*aN*aN;
    double rowOff = aP.coeffs[0] + aP.coeffs[1]*aN;
    double sx = col + colOff, sy = rowOff - (int)rowOff;
    int sRowBase = (int)row + (int)rowOff;

    if (sx < 0 || sx >= sW - 1) return {0, 0};
    int sY0 = sRowBase - readR; int sYH = readR * 2 + 1;
    if (sY0 < 0) { sYH += sY0; sY0 = 0; }
    if (sY0 + sYH > sH) sYH = sH - sY0;
    if (sYH <= 0) return {0, 0};

    auto sWin = reader->readBandWindow(0, 0, sY0, sW, sYH);
    return useSinc ? sincInterp2D(sWin, sW, sYH, sx, sy, sincW, beta)
                   : bilinearInterp2D(sWin, sW, sYH, sx, sy);
}

bool SincResampler::resampleTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    double prf     = p.masterPrf;                       // Hz
    double kt      = ctx.data.slaveAzimuthFmRate;       // Hz/s (负值)
    bool doDeramp  = (std::abs(kt) > 1e-6) && (prf > 0);
    int blendHalf  = qMin(80, L / 6);

    if (ctx.burstResults.size() < N) {
        ctx.errorMessage = "SincResampler: burstResults not populated"; return false;
    }

    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    writer.setGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    qDebug() << QStringLiteral("[Step9] TOPSAR resample %1x%2 %3bursts method=%4 deramp=%5 blend=%6")
        .arg(mW).arg(mH).arg(N).arg(p.resamplingMethod)
        .arg(doDeramp ? "on" : "off").arg(blendHalf);

    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2...").arg(b+1).arg(N);
        const auto& br = ctx.burstResults[b];
        int startRow = b * L;

        for (int r = 0; r < L; ++r) {
            int gRow = startRow + r;

            // ── 当前 burst 内插 + deramp ──
            QVector<std::complex<float>> rowBuf(mW);
            double aLoc = (double)gRow / mH;

            // deramp: η = 相对burst中心的方位时间 (秒)
            double eta = (doDeramp) ? (r - L/2.0) / prf : 0.0;
            double derampPhase = (doDeramp) ? -M_PI * kt * eta * eta : 0.0;
            float dCos = (float)std::cos(derampPhase);
            float dSin = (float)std::sin(derampPhase);

            for (int c = 0; c < mW; ++c) {
                auto val = interpSlave(ctx.slaveReader, sW, sH,
                    br.rangePoly, br.aziPoly, (double)c, (double)gRow, mW, mH,
                    useSinc, sincW, beta, readR);
                if (doDeramp && val != std::complex<float>{0, 0}) {
                    float re = val.real() * dCos - val.imag() * dSin;
                    float im = val.real() * dSin + val.imag() * dCos;
                    val = {re, im};
                }
                rowBuf[c] = val;
            }

            // ── Overlap 融合 ──
            bool inUpper = (b > 0 && r < blendHalf);
            bool inLower = (b + 1 < N && r >= L - blendHalf);

            if (inUpper) {
                // blend: prev burst (b-1) → current burst (b)
                double t = (double)(r + 1) / (blendHalf + 1);   // 0→1
                const auto& brP = ctx.burstResults[b - 1];
                for (int c = 0; c < mW; ++c) {
                    auto valP = interpSlave(ctx.slaveReader, sW, sH,
                        brP.rangePoly, brP.aziPoly, (double)c, (double)gRow, mW, mH,
                        useSinc, sincW, beta, readR);
                    if (doDeramp && valP != std::complex<float>{0, 0}) {
                        float re = valP.real() * dCos - valP.imag() * dSin;
                        float im = valP.real() * dSin + valP.imag() * dCos;
                        valP = {re, im};
                    }
                    float wP = (float)(1.0 - t), wC = (float)t;
                    rowBuf[c] = {wP * valP.real() + wC * rowBuf[c].real(),
                                 wP * valP.imag() + wC * rowBuf[c].imag()};
                }
            } else if (inLower) {
                // blend: current burst (b) → next burst (b+1)
                double t = (double)(r - (L - blendHalf) + 1) / (blendHalf + 1);  // 0→1
                const auto& brN = ctx.burstResults[b + 1];
                for (int c = 0; c < mW; ++c) {
                    auto valN = interpSlave(ctx.slaveReader, sW, sH,
                        brN.rangePoly, brN.aziPoly, (double)c, (double)gRow, mW, mH,
                        useSinc, sincW, beta, readR);
                    if (doDeramp && valN != std::complex<float>{0, 0}) {
                        float re = valN.real() * dCos - valN.imag() * dSin;
                        float im = valN.real() * dSin + valN.imag() * dCos;
                        valN = {re, im};
                    }
                    float wC = (float)(1.0 - t), wN = (float)t;
                    rowBuf[c] = {wC * rowBuf[c].real() + wN * valN.real(),
                                 wC * rowBuf[c].imag() + wN * valN.imag()};
                }
            }
            writer.writeRow(gRow, rowBuf);
        }
        if (b % 2 == 0) QApplication::processEvents();
    }
    return true;
}

bool SincResampler::execute(PipelineContext& ctx) {
    if (ctx.isTopsar) return resampleTopsar(ctx);
    else return resampleNonTopsar(ctx);
}
