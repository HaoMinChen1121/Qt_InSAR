#include "EsdCorrector.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <complex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── 用当前模型对辅影像窗口做双线性重采样到主网格 ──
// 消除已建模偏移后, ESD 相位只反映残余方位失配
// (插值偏差在两 burst 窗口间相同, 在 ESD 差分中抵消)
static void resampleEsdWindow(
    const QVector<std::complex<float>>& sWin, int colW, int ovLines,
    int col0, int mLineTop, int sLineTop, int mW, int mH,
    const RangePolynomial& rP, const AzimuthPolynomial& aP,
    QVector<std::complex<float>>& out)
{
    out.resize(colW * ovLines);
    for (int k = 0; k < colW * ovLines; ++k) {
        int cc = k % colW;
        int rr = k / colW;
        int col = col0 + cc;
        int mLine = mLineTop + rr;
        double rN = (double)col / mW, aN = (double)mLine / mH;
        double rangeOff = rP.coeffs[0]+rP.coeffs[1]*rN+rP.coeffs[2]*aN
            +rP.coeffs[3]*rN*aN+rP.coeffs[4]*rN*rN+rP.coeffs[5]*aN*aN;
        double aziOff = aP.coeffs[0]+aP.coeffs[1]*aN+aP.coeffs[2]*rN;
        // 辅窗口内坐标 (窗口原点 = (col0, sLineTop))
        double sx = cc + rangeOff;
        double sy = rr + (mLineTop - sLineTop) + aziOff;
        if (sx < 0 || sy < 0 || sx > colW - 2 || sy > ovLines - 2) { out[k] = {0, 0}; continue; }
        int x0 = (int)sx, y0 = (int)sy;
        double fx = sx - x0, fy = sy - y0;
        const auto& at = [&](int x, int y) { return sWin[y * colW + x]; };
        float w00 = (float)((1.0-fx)*(1.0-fy)), w10 = (float)(fx*(1.0-fy));
        float w01 = (float)((1.0-fx)*fy),       w11 = (float)(fx*fy);
        std::complex<float> v =
            at(x0, y0) * w00 + at(x0+1, y0) * w10
          + at(x0, y0+1) * w01 + at(x0+1, y0+1) * w11;
        out[k] = v;
    }
}

bool EsdCorrector::execute(PipelineContext& ctx) {
    if (!ctx.strategy || ctx.strategy->azimuthCorrection != AzimuthCorrection::ESD)
        return true;
    if (!ctx.isTopsar) return true;

    int N = ctx.data.burstCount;
    int L = ctx.data.linesPerBurst;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    Q_UNUSED(sW);

    const auto& burstStarts = ctx.data.burstStartLines;
    if (N < 2 || L < 100) return true;

    double deltaF = ctx.params->deltaFdoppler;
    double prf = ctx.data.masterAzimuthFrequency > 0
        ? ctx.data.masterAzimuthFrequency : ctx.params->masterPrf;
    if (deltaF == 0.0 && prf > 0 && ctx.data.linesPerBurst > 0) {
        // 数据链回退: Δf_dc ≈ kt × T_burst
        // (TOPS 相邻 burst 在重叠区观测到的多普勒质心差, 由 annotation XML 的
        //  azimuthFmRate/linesPerBurst/azimuthFrequency 推导)
        deltaF = ctx.data.slaveAzimuthFmRate * ctx.data.linesPerBurst / prf;
    }
    if (std::abs(deltaF) < 1e-6 || prf <= 0) {
        qWarning() << "[Step8] ESD skipped: deltaFdoppler/prf unavailable"
                   << "deltaF=" << deltaF << "prf=" << prf;
        return true;
    }
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

        // 辅影像重叠区行号: 按 burstPairs 匹配结果取对应辅 burst 的起始行
        int sLineA = lineA, sLineB = lineB;
        int j0 = b - 1, j1 = b;
        int sN = ctx.slaveBand->burstCount > 0 ? ctx.slaveBand->burstCount : N;
        if (ctx.burstPairs.size() == N) {
            if (ctx.burstPairs[j0].isValid && ctx.burstPairs[j0].slaveBurstIdx >= 0
                && ctx.burstPairs[j0].slaveBurstIdx < sN)
                j0 = ctx.burstPairs[j0].slaveBurstIdx;
            if (ctx.burstPairs[j1].isValid && ctx.burstPairs[j1].slaveBurstIdx >= 0
                && ctx.burstPairs[j1].slaveBurstIdx < sN)
                j1 = ctx.burstPairs[j1].slaveBurstIdx;
        }
        const auto& sStarts = ctx.slaveBand->burstStartLines;
        if (sStarts.size() > qMax(j0, j1)) {
            sLineA = sStarts[j0] + L - ovLines / 2;
            sLineB = sStarts[j1] + ovLines / 2;
        }
        sLineA = qBound(halfW, sLineA, sH - halfW);
        sLineB = qBound(halfW, sLineB, sH - halfW);

        QVector<std::complex<float>> mA(colW * ovLines);
        QVector<std::complex<float>> sA(colW * ovLines);
        QVector<std::complex<float>> mB(colW * ovLines);
        QVector<std::complex<float>> sB(colW * ovLines);

        if (ctx.useBurstCache) {
            ctx.masterSdr->readWindow(col0, lineA - ovLines/2, colW, ovLines, mA.data());
            ctx.slaveSdr->readWindow(col0, sLineA - ovLines/2, colW, ovLines, sA.data());
            ctx.masterSdr->readWindow(col0, lineB - ovLines/2, colW, ovLines, mB.data());
            ctx.slaveSdr->readWindow(col0, sLineB - ovLines/2, colW, ovLines, sB.data());
        } else {
            mA = ctx.masterReader->readBandWindow(0, col0, lineA - ovLines/2, colW, ovLines);
            sA = ctx.slaveReader->readBandWindow(0, col0, sLineA - ovLines/2, colW, ovLines);
            mB = ctx.masterReader->readBandWindow(0, col0, lineB - ovLines/2, colW, ovLines);
            sB = ctx.slaveReader->readBandWindow(0, col0, sLineB - ovLines/2, colW, ovLines);
        }

        if (mA.size() < colW*ovLines || sA.size() < colW*ovLines
            || mB.size() < colW*ovLines || sB.size() < colW*ovLines) {
            relCorr[b] = 0; continue;
        }

        // 模型重采样: 辅窗口对齐到主网格后, ESD 相位只反映残余方位失配
        QVector<std::complex<float>> sAr, sBr;
        resampleEsdWindow(sA, colW, ovLines, col0, lineA - ovLines/2, sLineA - ovLines/2,
                          mW, mH, ctx.rangePoly, ctx.aziPoly, sAr);
        resampleEsdWindow(sB, colW, ovLines, col0, lineB - ovLines/2, sLineB - ovLines/2,
                          mW, mH, ctx.rangePoly, ctx.aziPoly, sBr);

        std::complex<double> esdSum(0, 0);
        for (int k = 0; k < colW * ovLines; ++k) {
            auto ifgA = std::complex<double>(mA[k].real(), mA[k].imag())
                       * std::complex<double>(sAr[k].real(), -sAr[k].imag());
            auto ifgB = std::complex<double>(mB[k].real(), mB[k].imag())
                       * std::complex<double>(sBr[k].real(), -sBr[k].imag());
            esdSum += ifgA * std::conj(ifgB);
        }
        double phase = std::arg(esdSum);
        relCorr[b] = phase / (2.0 * M_PI * deltaF / prf);
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
