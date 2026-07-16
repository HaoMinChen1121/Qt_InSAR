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

// ── 计算一行中所有像素的 slave 坐标 + 行偏移 ──
struct RowCoords {
    QVector<double> sx;       // slave col = col + colOff
    QVector<double> syFrac;   // 亚像素行偏移
    int sRowBase;
    int sY0, sYH;             // 条带窗口
};

static RowCoords computeRowCoords(int gRow, int mW, int mH, int sH,
    const RangePolynomial& rP, const AzimuthPolynomial& aP, int readR)
{
    RowCoords rc;
    rc.sx.resize(mW);
    rc.syFrac.resize(mW);
    double aLoc = (double)gRow / mH;
    double rowOff = aP.coeffs[0] + aP.coeffs[1] * aLoc;
    rc.sRowBase = gRow + (int)rowOff;
    double syFracBase = rowOff - (int)rowOff;

    rc.sY0 = rc.sRowBase - readR; rc.sYH = readR * 2 + 1;
    if (rc.sY0 < 0) { rc.sYH += rc.sY0; rc.sY0 = 0; }
    if (rc.sY0 + rc.sYH > sH) rc.sYH = sH - rc.sY0;

    for (int c = 0; c < mW; ++c) {
        double rN = (double)c / mW;
        double colOff = rP.coeffs[0] + rP.coeffs[1]*rN + rP.coeffs[2]*aLoc
                      + rP.coeffs[3]*rN*aLoc + rP.coeffs[4]*rN*rN + rP.coeffs[5]*aLoc*aLoc;
        rc.sx[c] = c + colOff;
        rc.syFrac[c] = syFracBase;
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

    double prf     = p.masterPrf;
    double kt      = ctx.data.slaveAzimuthFmRate;
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

    int step = std::max(1, mH / 100);

    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2...").arg(b+1).arg(N);
        const auto& br = ctx.burstResults[b];
        int startRow = b * L;

        for (int r = 0; r < L; ++r) {
            int gRow = startRow + r;
            QVector<std::complex<float>> rowBuf(mW);

            // ── 当前burst: 读一整行条带，全像素内插 ──
            auto rcCur = computeRowCoords(gRow, mW, mH, sH,
                br.rangePoly, br.aziPoly, readR);

            double eta = (doDeramp) ? (r - L/2.0) / prf : 0.0;
            double derampPhase = (doDeramp) ? -M_PI * kt * eta * eta : 0.0;
            float dCos = (float)std::cos(derampPhase);
            float dSin = (float)std::sin(derampPhase);

            if (rcCur.sYH <= 0) {
                rowBuf.fill({0, 0});
            } else {
                auto stripCur = ctx.slaveReader->readBandWindow(0, 0, rcCur.sY0, sW, rcCur.sYH);
                for (int c = 0; c < mW; ++c) {
                    auto val = interpFromStrip(stripCur, sW, rcCur.sYH,
                        rcCur.sx[c], rcCur.syFrac[c], useSinc, sincW, beta);
                    if (doDeramp && val != std::complex<float>{0, 0}) {
                        float re = val.real() * dCos - val.imag() * dSin;
                        float im = val.real() * dSin + val.imag() * dCos;
                        val = {re, im};
                    }
                    rowBuf[c] = val;
                }
            }

            // ── Overlap 融合: 读相邻burst的条带，逐像素加权叠加 ──
            bool inUpper = (b > 0 && r < blendHalf);
            bool inLower = (b + 1 < N && r >= L - blendHalf);

            if (inUpper) {
                double t = (double)(r + 1) / (blendHalf + 1);
                const auto& brP = ctx.burstResults[b - 1];
                auto rcPrev = computeRowCoords(gRow, mW, mH, sH,
                    brP.rangePoly, brP.aziPoly, readR);
                if (rcPrev.sYH > 0) {
                    auto stripPrev = ctx.slaveReader->readBandWindow(0, 0, rcPrev.sY0, sW, rcPrev.sYH);
                    float wP = (float)(1.0 - t), wC = (float)t;
                    for (int c = 0; c < mW; ++c) {
                        auto valP = interpFromStrip(stripPrev, sW, rcPrev.sYH,
                            rcPrev.sx[c], rcPrev.syFrac[c], useSinc, sincW, beta);
                        if (doDeramp && valP != std::complex<float>{0, 0}) {
                            float re = valP.real() * dCos - valP.imag() * dSin;
                            float im = valP.real() * dSin + valP.imag() * dCos;
                            valP = {re, im};
                        }
                        rowBuf[c] = {wP * valP.real() + wC * rowBuf[c].real(),
                                     wP * valP.imag() + wC * rowBuf[c].imag()};
                    }
                }
            } else if (inLower) {
                double t = (double)(r - (L - blendHalf) + 1) / (blendHalf + 1);
                const auto& brN = ctx.burstResults[b + 1];
                auto rcNext = computeRowCoords(gRow, mW, mH, sH,
                    brN.rangePoly, brN.aziPoly, readR);
                if (rcNext.sYH > 0) {
                    auto stripNext = ctx.slaveReader->readBandWindow(0, 0, rcNext.sY0, sW, rcNext.sYH);
                    float wC = (float)(1.0 - t), wN = (float)t;
                    for (int c = 0; c < mW; ++c) {
                        auto valN = interpFromStrip(stripNext, sW, rcNext.sYH,
                            rcNext.sx[c], rcNext.syFrac[c], useSinc, sincW, beta);
                        if (doDeramp && valN != std::complex<float>{0, 0}) {
                            float re = valN.real() * dCos - valN.imag() * dSin;
                            float im = valN.real() * dSin + valN.imag() * dCos;
                            valN = {re, im};
                        }
                        rowBuf[c] = {wC * rowBuf[c].real() + wN * valN.real(),
                                     wC * rowBuf[c].imag() + wN * valN.imag()};
                    }
                }
            }
            writer.writeRow(gRow, rowBuf);
            if (gRow % step == 0) QApplication::processEvents();
        }
        if (b % 2 == 0) QApplication::processEvents();
    }
    return true;
}

bool SincResampler::execute(PipelineContext& ctx) {
    if (ctx.isTopsar) return resampleTopsar(ctx);
    else return resampleNonTopsar(ctx);
}
