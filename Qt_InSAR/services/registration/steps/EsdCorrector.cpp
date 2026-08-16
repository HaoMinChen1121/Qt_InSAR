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
    // 距离分箱: 全宽 8 箱 (ESD 相位逐箱测量 → 方位修正随距离变化)
    // 2026-08-15: TOPS 方位残余随距离变化 (零多普勒几何), 单一常数修正
    // 只修中间部分 → 部分距离处残余 ~0.1-0.5 行摧毁像素级相干;
    // 逐箱测 δa(r) 后拟合 AzimuthPolynomial 的 r 项
    const int nBins = 8;
    const int binW = mW / nBins;
    const int col0 = 0, colW = binW * nBins;

    ctx.burstResults.resize(N);

    // ── 逐 burst 方位常数 (2026-08-16 第十九轮定案) ──
    // ANX 初值携带两个雷达 burst 网格的真实整数相位差 (实测逐 burst
    // 1.3532/0.3532/2.3532 三档, 档差恰 1px) — 全局多项式拟合会把它
    // 平滑掉 → 重采样方位错位 ~1px (post-coreg 0.23-1.10px, TOPS 要求
    // <0.01px) → 相干性摧毁。burstResults 的 aziPoly 逐 burst 用常数
    // (ESD α/β 叠加其上); rangePoly 保持全局多项式 (range 偏移随距离/
    // 方位平滑变化, 多项式建模正确)。
    QVector<AzimuthPolynomial> perBurstAzi(N);
    for (int b = 0; b < N; ++b) {
        perBurstAzi[b] = ctx.aziPoly;
        if (ctx.initialOffsets.size() > b) {
            perBurstAzi[b].coeffs[0] = ctx.initialOffsets[b].aziOff;
            perBurstAzi[b].coeffs[1] = 0.0;
            perBurstAzi[b].coeffs[2] = 0.0;
        }
    }

    QVector<QVector<double>> relCorr(N, QVector<double>(nBins, 0.0));
    QVector<QVector<double>> esdMag(N, QVector<double>(nBins, 0.0));  // 逐箱相干和幅度 (SNR 权重)

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
            continue;
        }

        // 模型重采样: 辅窗口对齐到主网格后, ESD 相位只反映残余方位失配
        // (方位模型用各自 burst 的常数 — 第十九轮: 全局多项式会引入 ±1px)
        QVector<std::complex<float>> sAr, sBr;
        resampleEsdWindow(sA, colW, ovLines, col0, lineA - ovLines/2, sLineA - ovLines/2,
                          mW, mH, ctx.rangePoly, perBurstAzi[b - 1], sAr);
        resampleEsdWindow(sB, colW, ovLines, col0, lineB - ovLines/2, sLineB - ovLines/2,
                          mW, mH, ctx.rangePoly, perBurstAzi[b], sBr);

        // 逐距离箱 ESD 相位
        for (int bin = 0; bin < nBins; ++bin) {
            std::complex<double> esdSum(0, 0);
            for (int rr = 0; rr < ovLines; ++rr) {
                const int rowOff = rr * colW;
                for (int cc = bin * binW; cc < (bin + 1) * binW; ++cc) {
                    const int k = rowOff + cc;
                    auto ifgA = std::complex<double>(mA[k].real(), mA[k].imag())
                               * std::complex<double>(sAr[k].real(), -sAr[k].imag());
                    auto ifgB = std::complex<double>(mB[k].real(), mB[k].imag())
                               * std::complex<double>(sBr[k].real(), -sBr[k].imag());
                    esdSum += ifgA * std::conj(ifgB);
                }
            }
            const double phase = std::arg(esdSum);
            relCorr[b][bin] = phase / (2.0 * M_PI * deltaF / prf);
            esdMag[b][bin] = std::abs(esdSum);
        }
    }

    // 累积绝对修正 (逐箱独立链), 异常值过滤
    QVector<QVector<double>> absCorr(N, QVector<double>(nBins, 0.0));
    for (int b = 1; b < N; ++b) {
        for (int bin = 0; bin < nBins; ++bin) {
            absCorr[b][bin] = absCorr[b-1][bin] + relCorr[b][bin];
            if (std::abs(absCorr[b][bin]) > 1.0) absCorr[b][bin] = 0;
        }
    }

    // 每 burst: 对逐箱修正做 SNR 加权线性拟合 corr(rN) = α + β·rN
    // → 逐 burst 方位常数 += α, r 项 += β (第十九轮: aziPoly = [initAzi_b,0,0]+ESD;
    //   rangePoly 保持全局多项式 — 第十八轮 #1 失败是因为把 range 也改成
    //   逐 burst 常数 (range 偏移随距离/方位平滑变化, 常数建模错误))
    for (int b = 0; b < N; ++b) {
        ctx.burstResults[b].burstIndex = b;
        ctx.burstResults[b].rangePoly  = ctx.rangePoly;
        ctx.burstResults[b].aziPoly    = perBurstAzi[b];
        if (b == 0) continue;

        // 权重 = 该 burst 各箱 esdMag 的累积 (跨 seam 平均)
        double sw = 0, swx = 0, swy = 0, swxx = 0, swxy = 0;
        int nv = 0;
        for (int bin = 0; bin < nBins; ++bin) {
            if (std::abs(absCorr[b][bin]) < 1e-9) continue;  // 过滤后的无效箱
            const double rN = (bin + 0.5) * binW / mW;
            double wgt = esdMag[b][bin];
            for (int bb = b + 1; bb < N; ++bb) wgt += esdMag[bb][bin];
            if (wgt < 1e-9) wgt = 1.0;
            sw += wgt; swx += wgt * rN; swy += wgt * absCorr[b][bin];
            swxx += wgt * rN * rN; swxy += wgt * rN * absCorr[b][bin];
            ++nv;
        }
        if (nv >= 4 && std::abs(sw * swxx - swx * swx) > 1e-12) {
            const double beta = (sw * swxy - swx * swy) / (sw * swxx - swx * swx);
            const double alpha = (swy - beta * swx) / sw;
            ctx.burstResults[b].aziPoly.coeffs[0] += alpha;
            ctx.burstResults[b].aziPoly.coeffs[2] += beta;
            if (b == N / 2 || std::abs(beta) > 0.5)
                qDebug() << "[Step8] burst" << b << "ESD fit: alpha="
                         << QString::number(alpha, 'f', 4) << " beta="
                         << QString::number(beta, 'f', 4) << "(r-term, px per normalized range)";
        } else {
            // 有效箱不足: 回退到加权均值 (常数项)
            double sum = 0, wsum = 0;
            for (int bin = 0; bin < nBins; ++bin)
                if (std::abs(absCorr[b][bin]) >= 1e-9) {
                    double wgt = esdMag[b][bin];
                    if (wgt < 1e-9) wgt = 1.0;
                    sum += absCorr[b][bin] * wgt; wsum += wgt;
                }
            if (wsum > 0)
                ctx.burstResults[b].aziPoly.coeffs[0] += sum / wsum;
        }
    }
    ctx.esdApplied = true;
    qDebug() << "[Step8] ESD adjusted" << N << "burst azimuth corrections"
             << "(range-binned," << nBins << "bins, constant+r-term)";
    return true;
}
