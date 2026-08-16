#include "QualityEvaluator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// ── 配准后互相关验证: 主影像 vs 注册辅影像 (振幅域) ──
// 3 个 512² 窗 (中 burst, 近/中/远距), FFT 幅度相关 ±4px 中心峰搜索
// (防场景周期旁瓣劫持 — 2026-08-15 喀什农田 ±10px 旁瓣教训) +
// 抛物线亚像素。峰值位置 = 残余失配 (亚像素), 峰值高度只记录不设门
// (12 天对振幅相关 ~0.1, 是场景/时间基线函数, 非配准指标)。
void postCoregCheck(PipelineContext& ctx, QualityReport& r)
{
    if (ctx.outputPath.isEmpty() || !QFileInfo::exists(ctx.outputPath))
        return;
    const QString mPath = ctx.masterLocalPath.isEmpty()
        ? ctx.masterBand->rasterPath : ctx.masterLocalPath;
    GdalSlcReader mR, sR;
    if (!mR.open(mPath) || !sR.open(ctx.outputPath)) {
        qWarning() << "[Step10] post-coreg check: cannot open"
                   << mPath << "/" << ctx.outputPath;
        return;
    }
    const int w = mR.width(), h = mR.height();
    const int N = ctx.data.burstCount, L = std::max(1, ctx.data.linesPerBurst);
    const int bMid = N / 2;
    const int rowC = (ctx.data.burstStartLines.size() > bMid
                      ? ctx.data.burstStartLines[bMid] : bMid * L) + L / 2;
    constexpr int win = 512, halfSearch = 4;
    const double colFracs[3] = {0.25, 0.5, 0.75};

    int nOk = 0;
    double sumDR = 0, sumDA = 0, sumPk = 0;
    std::vector<float> surf(static_cast<size_t>(2 * win - 1) * (2 * win - 1));
    const int outDim = 2 * win - 1;

    for (double cf : colFracs) {
        int col0 = static_cast<int>(w * cf) - win / 2;
        int row0 = rowC - win / 2;
        if (col0 < 0 || row0 < 0 || col0 + win > w || row0 + win > h)
            continue;
        auto mA = mR.readBandWindow(0, col0, row0, win, win);
        auto sA = sR.readBandWindow(0, col0, row0, win, win);
        if (mA.size() != win * win || sA.size() != win * win)
            continue;
        double eM = 0, eS = 0;
        for (int i = 0; i < win * win; ++i) {
            eM += std::norm(mA[i]);
            eS += std::norm(sA[i]);
        }
        const float maxV = fftAmpCorrelate(mA.constData(), sA.constData(),
                                           surf.data(), win, win);
        if (maxV <= 0 || eM <= 0 || eS <= 0)
            continue;
        // 中心 ±4px 峰搜索 (lag(0,0) 在表面中心)
        const int cC = win - 1, cR = win - 1;
        float best = -1e9f;
        int bc = 0, br = 0;
        for (int dr = -halfSearch; dr <= halfSearch; ++dr)
            for (int dc = -halfSearch; dc <= halfSearch; ++dc) {
                const float v = surf[static_cast<size_t>(cR + dr) * outDim + (cC + dc)];
                if (v > best) { best = v; br = dr; bc = dc; }
            }
        // 抛物线亚像素 (峰滞后 = 残余偏移)
        double dC = 0, dR = 0;
        {
            const int pr = cR + br, pc = cC + bc;
            if (pc > 0 && pc < outDim - 1) {
                const float f0 = surf[static_cast<size_t>(pr) * outDim + pc];
                const float fm = surf[static_cast<size_t>(pr) * outDim + (pc - 1)];
                const float fp = surf[static_cast<size_t>(pr) * outDim + (pc + 1)];
                dC = static_cast<double>(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
            }
            if (pr > 0 && pr < outDim - 1) {
                const float f0 = surf[static_cast<size_t>(pr) * outDim + pc];
                const float fm = surf[static_cast<size_t>(pr - 1) * outDim + pc];
                const float fp = surf[static_cast<size_t>(pr + 1) * outDim + pc];
                dR = static_cast<double>(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
            }
        }
        const double resR = bc + dC;   // 列滞后 = 残余距离偏移
        const double resA = br + dR;   // 行滞后 = 残余方位偏移
        const double normPk = static_cast<double>(best) / std::sqrt(eM * eS);
        r.postCoregWindowRangeRes.append(resR);
        r.postCoregWindowAziRes.append(resA);
        r.postCoregWindowPeak.append(normPk);
        sumDR += resR; sumDA += resA; sumPk += normPk;
        ++nOk;
    }
    if (nOk > 0) {
        r.postCoregResidualRange   = sumDR / nOk;
        r.postCoregResidualAzimuth = sumDA / nOk;
        r.postCoregPeakValue       = sumPk / nOk;
        r.postCoregWindows         = nOk;
    }
    qDebug().nospace() << "[Step10] post-coreg check: windows=" << nOk
        << " residualR=" << QString::number(r.postCoregResidualRange, 'f', 3)
        << "px residualA=" << QString::number(r.postCoregResidualAzimuth, 'f', 3)
        << "px peak=" << QString::number(r.postCoregPeakValue, 'f', 3);
}

} // namespace

bool QualityEvaluator::execute(PipelineContext& ctx) {
    QualityReport& r = ctx.qualityReport;
    r.totalPoints = ctx.offsetPoints.size();

    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useBurstPoly = ctx.esdApplied && N > 1 && !ctx.burstResults.isEmpty();

    // ── 偏移RMSE + 残差 mean/std (区分系统偏差与噪声) ──
    double sumSq = 0, sumSqR = 0, sumSqA = 0; int cnt = 0;
    double sumR = 0, sumA = 0, sumRn = 0, sumRn2 = 0, sumRnA = 0;
    for (const auto& pt : ctx.offsetPoints) {
        if (pt.correlation <= 0) continue;
        double rn = (double)pt.col / w, an = (double)pt.row / h;
        double predR = 0, predA = 0;

        if (useBurstPoly) {
            // TOPSAR ESD: 使用逐burst多项式
            int bIdx = pt.row / L;
            if (bIdx < 0 || bIdx >= N) bIdx = qBound(0, bIdx, N - 1);
            const auto& rP = ctx.burstResults[bIdx].rangePoly;
            const auto& aP = ctx.burstResults[bIdx].aziPoly;
            predR = rP.coeffs[0] + rP.coeffs[1]*rn + rP.coeffs[2]*an
                  + rP.coeffs[3]*rn*an + rP.coeffs[4]*rn*rn + rP.coeffs[5]*an*an;
            predA = aP.coeffs[0] + aP.coeffs[1]*an + aP.coeffs[2]*rn;
        } else {
            predR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn
                  + ctx.rangePoly.coeffs[2]*an + ctx.rangePoly.coeffs[3]*rn*an
                  + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
            predA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an
                  + ctx.aziPoly.coeffs[2]*rn;
        }
        double resR = pt.rangeOff - predR, resA = pt.aziOff - predA;
        sumSq += resR*resR + resA*resA;
        sumSqR += resR*resR; sumSqA += resA*resA;
        sumR += resR; sumA += resA;
        sumRn += rn; sumRn2 += rn * rn; sumRnA += rn * resA;
        ++cnt;
    }
    r.validPoints = cnt;
    r.offsetRmse = cnt > 0 ? std::sqrt(sumSq / cnt) : 0;
    r.rangeRmse  = cnt > 0 ? std::sqrt(sumSqR / cnt) : 0;
    r.aziRmse    = cnt > 0 ? std::sqrt(sumSqA / cnt) : 0;
    if (cnt > 0) {
        const double meanR = sumR / cnt, meanA = sumA / cnt;
        r.rangeOffsetMean = meanR;
        r.aziOffsetMean   = meanA;
        r.rangeOffsetStd  = std::sqrt(std::max(0.0, sumSqR / cnt - meanR * meanR));
        r.aziOffsetStd    = std::sqrt(std::max(0.0, sumSqA / cnt - meanA * meanA));
        // 方位残差-vs-距离斜率: Σ(rn−rn̄)(resA−resĀ)/Σ(rn−rn̄)²
        const double denom = sumRn2 - sumRn * sumRn / cnt;
        if (denom > 1e-12)
            r.aziResidualRangeSlope = (sumRnA - sumRn * meanA) / denom;
    }
    // 模型方位 r 项 (ESD β): 逐 burst 多项式取平均, 否则全局多项式
    {
        double c2 = 0; int nb = 0;
        if (useBurstPoly) {
            for (const auto& br : ctx.burstResults) { c2 += br.aziPoly.coeffs[2]; ++nb; }
            r.aziPolyRangeCoeff = nb > 0 ? c2 / nb : 0.0;
        } else {
            r.aziPolyRangeCoeff = ctx.aziPoly.coeffs[2];
        }
    }
    // 多项式拟合质量 (Step6, ESD 前)
    r.polyRangeRmse = ctx.rangePoly.rmse;
    r.polyAziRmse   = ctx.aziPoly.rmse;

    // ── 平均相关系数 (仅NCC路线有效, FFT路线用多项式RMSE) ──
    // FFT 路线的 pt.correlation 是未归一化的相关峰幅度 (可达 1e8),
    // 直接平均会产生无意义的质量摘要值 — 按路线门控
    int vc = 0; double sumCorr = 0;
    if (ctx.strategy && ctx.strategy->coarseCorr == CorrelationMethod::NCC) {
        for (const auto& pt : ctx.offsetPoints) {
            if (pt.correlation > 0) { sumCorr += pt.correlation; ++vc; }
        }
    }
    r.meanCorrelation = vc > 0 ? sumCorr / vc : 0;

    // ── ESD残差 ──
    r.esdMaxResidual = 0;
    if (ctx.esdApplied && N > 1) {
        for (int b = 1; b < N; ++b) {
            double d0 = ctx.burstResults[b-1].aziPoly.coeffs[0];
            double d1 = ctx.burstResults[b].aziPoly.coeffs[0];
            double delta = std::abs(d1 - d0);
            r.esdPhaseDeltas.append(delta);
            if (delta > r.esdMaxResidual) r.esdMaxResidual = delta;
        }
    }

    // ── Per-burst RMSE (2D + 距离/方位分离) ──
    for (int b = 0; b < N; ++b) {
        double ss = 0, ssR = 0, ssA = 0; int nc = 0;
        int startRow = ctx.data.burstStartLines.isEmpty()
            ? b * L : ctx.data.burstStartLines[b];
        int endRow = startRow + L;
        for (const auto& pt : ctx.offsetPoints) {
            if (pt.row < startRow || pt.row >= endRow || pt.correlation <= 0) continue;
            double rn = (double)pt.col / w, an = (double)pt.row / h;
            double pR = 0, pA = 0;
            if (useBurstPoly) {
                const auto& rP = ctx.burstResults[b].rangePoly;
                const auto& aP = ctx.burstResults[b].aziPoly;
                pR = rP.coeffs[0] + rP.coeffs[1]*rn + rP.coeffs[2]*an
                   + rP.coeffs[3]*rn*an + rP.coeffs[4]*rn*rn + rP.coeffs[5]*an*an;
                pA = aP.coeffs[0] + aP.coeffs[1]*an + aP.coeffs[2]*rn;
            } else {
                pR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn
                   + ctx.rangePoly.coeffs[2]*an + ctx.rangePoly.coeffs[3]*rn*an
                   + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
                pA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an
                   + ctx.aziPoly.coeffs[2]*rn;
            }
            const double dr = pt.rangeOff - pR, da = pt.aziOff - pA;
            ss += dr*dr + da*da; ssR += dr*dr; ssA += da*da; ++nc;
        }
        r.perBurstRmse.append(nc > 0 ? std::sqrt(ss / nc) : 0);
        r.perBurstRangeRmse.append(nc > 0 ? std::sqrt(ssR / nc) : 0);
        r.perBurstAziRmse.append(nc > 0 ? std::sqrt(ssA / nc) : 0);
    }

    // ── 完成标志 + 配准后互相关验证 ──
    r.esdDone = ctx.esdApplied;
    r.resamplingDone = QFileInfo::exists(ctx.outputPath);
    postCoregCheck(ctx, r);

    // ── 质量等级 ──
    // 多项式RMSE <0.05为GOOD, <0.5为OK, >=0.5为POOR
    double polyRmse = ctx.rangePoly.rmse;  // Step6拟合时计算好的
    QString level = (polyRmse < 0.05) ? "GOOD" : (polyRmse < 0.5 ? "OK" : "POOR");
    qDebug() << QStringLiteral("[Step10] Quality: polyRMSE=%1 (%2) offsetRMSE=%3 ESDmax=%4 pts=%5/%6"
                               " meanR=%7 stdA=%8 slopeA_r=%9 postCoregR=%10/A=%11")
        .arg(polyRmse, 0, 'f', 4).arg(level)
        .arg(r.offsetRmse, 0, 'f', 4).arg(r.esdMaxResidual, 0, 'f', 4)
        .arg(r.validPoints).arg(r.totalPoints)
        .arg(r.rangeOffsetMean, 0, 'f', 4).arg(r.aziOffsetStd, 0, 'f', 4)
        .arg(r.aziResidualRangeSlope, 0, 'f', 4)
        .arg(r.postCoregResidualRange, 0, 'f', 3)
        .arg(r.postCoregResidualAzimuth, 0, 'f', 3);
    return true;
}
