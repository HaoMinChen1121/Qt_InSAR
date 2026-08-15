#include "QualityEvaluator.h"
#include "../PipelineContext.h"
#include <QDebug>
#include <cmath>

bool QualityEvaluator::execute(PipelineContext& ctx) {
    QualityReport& r = ctx.qualityReport;
    r.totalPoints = ctx.offsetPoints.size();

    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useBurstPoly = ctx.esdApplied && N > 1 && !ctx.burstResults.isEmpty();

    // ── 偏移RMSE (2D 合并 + 距离/方位分离诊断) ──
    double sumSq = 0, sumSqR = 0, sumSqA = 0; int cnt = 0;
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
        ++cnt;
    }
    r.validPoints = cnt;
    r.offsetRmse = cnt > 0 ? std::sqrt(sumSq / cnt) : 0;
    r.rangeRmse  = cnt > 0 ? std::sqrt(sumSqR / cnt) : 0;
    r.aziRmse    = cnt > 0 ? std::sqrt(sumSqA / cnt) : 0;
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

    // ── 质量等级 ──
    // 多项式RMSE <0.05为GOOD, <0.5为OK, >=0.5为POOR
    double polyRmse = ctx.rangePoly.rmse;  // Step6拟合时计算好的
    QString level = (polyRmse < 0.05) ? "GOOD" : (polyRmse < 0.5 ? "OK" : "POOR");
    qDebug() << QStringLiteral("[Step10] Quality: polyRMSE=%1 (%2) offsetRMSE=%3 ESDmax=%4 pts=%5/%6")
        .arg(polyRmse, 0, 'f', 4).arg(level)
        .arg(r.offsetRmse, 0, 'f', 4).arg(r.esdMaxResidual, 0, 'f', 4)
        .arg(r.validPoints).arg(r.totalPoints);
    return true;
}
