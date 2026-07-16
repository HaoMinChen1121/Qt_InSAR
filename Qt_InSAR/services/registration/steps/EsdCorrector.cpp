#include "EsdCorrector.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool EsdCorrector::execute(PipelineContext& ctx) {
    if (ctx.params->route == RegRoute::Route1_OrbitFFT) return true;
    if (!ctx.isTopsar || !ctx.params->enableEsd) return true;

    int N = ctx.data.burstCount;
    int L = ctx.data.linesPerBurst;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    Q_UNUSED(sW); Q_UNUSED(sH);

    const auto& burstStarts = ctx.data.burstStartLines;
    if (N < 2 || L < 100) return true;

    double deltaF = ctx.params->deltaFdoppler;
    int ovLines = ctx.params->esdOverlapLines > 0
        ? qMin(ctx.params->esdOverlapLines, L / 10) : L / 10;
    int halfW = 16;
    int col0 = mW / 4, colW = mW / 2;

    ctx.burstResults.resize(N);

    QVector<double> relCorr(N);
    relCorr[0] = 0.0;

    for (int b = 1; b < N; ++b) {
        if (mCancelled) return false;
        int lineA, lineB;
        if (burstStarts.size() > b) {
            lineA = burstStarts[b - 1] + L - ovLines / 2;
            lineB = burstStarts[b] + ovLines / 2;
        } else {
            lineA = (b - 1) * L + L - ovLines / 2;
            lineB = b * L + ovLines / 2;
        }
        lineA = qBound(halfW, lineA, mH - halfW);
        lineB = qBound(halfW, lineB, mH - halfW);

        auto mA = ctx.masterReader->readBandWindow(0, col0, lineA - ovLines/2, colW, ovLines);
        auto sA = ctx.slaveReader->readBandWindow(0, col0, lineA - ovLines/2, colW, ovLines);
        auto mB = ctx.masterReader->readBandWindow(0, col0, lineB - ovLines/2, colW, ovLines);
        auto sB = ctx.slaveReader->readBandWindow(0, col0, lineB - ovLines/2, colW, ovLines);

        if (mA.size() < colW*ovLines || sA.size() < colW*ovLines
            || mB.size() < colW*ovLines || sB.size() < colW*ovLines) {
            relCorr[b] = 0; continue;
        }

        std::complex<double> esdSum(0, 0);
        for (int k = 0; k < colW * ovLines; ++k) {
            auto ifgA = std::complex<double>(mA[k].real(), mA[k].imag())
                       * std::complex<double>(sA[k].real(), -sA[k].imag());
            auto ifgB = std::complex<double>(mB[k].real(), mB[k].imag())
                       * std::complex<double>(sB[k].real(), -sB[k].imag());
            esdSum += ifgA * std::conj(ifgB);
        }
        double phase = std::arg(esdSum);
        relCorr[b] = phase / (2.0 * M_PI * deltaF / ctx.params->masterPrf);
    }

    // 累积绝对修正, 更新每burst的aziPoly常数项
    QVector<double> absCorr(N);
    absCorr[0] = 0.0;
    for (int b = 1; b < N; ++b) {
        absCorr[b] = absCorr[b-1] + relCorr[b];
        if (std::abs(absCorr[b]) > 1.0) absCorr[b] = 0; // 异常值过滤
    }

    ctx.burstResults.resize(N);
    for (int b = 0; b < N; ++b) {
        ctx.burstResults[b].burstIndex = b;
        ctx.burstResults[b].rangePoly  = ctx.rangePoly;
        ctx.burstResults[b].aziPoly    = ctx.aziPoly;
        ctx.burstResults[b].aziPoly.coeffs[0] += absCorr[b]; // 仅调常数项
    }
    ctx.esdApplied = true;
    qDebug() << "[Step8] ESD adjusted" << N << "burst azimuth constants";
    return true;
}
