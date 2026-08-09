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
    qDebug() << "[Step8] ESD Corrector starting, isTopsar=" << ctx.isTopsar << "enableEsd=" << ctx.params->enableEsd;
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

        // 合并读取: 两窗口在 burst 边界处相邻, 一次大窗口覆盖
        // 用 ceiling division 避免 ovLines 为奇数时合并窗口少1行
        int halfLo = ovLines / 2;
        int halfHi = ovLines - halfLo;
        int mergeStart = lineA - halfLo;
        int mergeH     = (lineB + halfHi) - mergeStart;
        int offsetB    = lineB - lineA;  // B窗口在合并缓冲中的行偏移

        auto mMerged = ctx.masterReader->readBandWindow(0, col0, mergeStart, colW, mergeH);
        auto sMerged = ctx.slaveReader->readBandWindow(0, col0, mergeStart, colW, mergeH);

        if (mMerged.size() < colW * mergeH || sMerged.size() < colW * mergeH) {
            relCorr[b] = 0; continue;
        }

        std::complex<double> esdSum(0, 0);
        int baseB = offsetB * colW;
        int nPix = colW * ovLines;
        for (int k = 0; k < nPix; ++k) {
            auto ifgA = std::complex<double>(mMerged[k].real(), mMerged[k].imag())
                       * std::complex<double>(sMerged[k].real(), -sMerged[k].imag());
            auto ifgB = std::complex<double>(mMerged[baseB + k].real(), mMerged[baseB + k].imag())
                       * std::complex<double>(sMerged[baseB + k].real(), -sMerged[baseB + k].imag());
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
