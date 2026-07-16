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

    // ── 偏移RMSE ──
    double sumSq = 0; int cnt = 0;
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
            predA = aP.coeffs[0] + aP.coeffs[1]*an;
        } else {
            predR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn
                  + ctx.rangePoly.coeffs[2]*an + ctx.rangePoly.coeffs[3]*rn*an
                  + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
            predA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an;
        }
        double resR = pt.rangeOff - predR, resA = pt.aziOff - predA;
        sumSq += resR*resR + resA*resA; ++cnt;
    }
    r.validPoints = cnt;
    r.offsetRmse = cnt > 0 ? std::sqrt(sumSq / cnt) : 0;

    // ── 平均相关系数 (仅NCC路线有效, FFT路线用多项式RMSE) ──
    int vc = 0; double sumCorr = 0;
    for (const auto& pt : ctx.offsetPoints) {
        if (pt.correlation > 0) { sumCorr += pt.correlation; ++vc; }
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

    // ── Per-burst RMSE ──
    for (int b = 0; b < N; ++b) {
        double ss = 0; int nc = 0;
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
                pA = aP.coeffs[0] + aP.coeffs[1]*an;
            } else {
                pR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn
                   + ctx.rangePoly.coeffs[2]*an + ctx.rangePoly.coeffs[3]*rn*an
                   + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
                pA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an;
            }
            ss += (pt.rangeOff-pR)*(pt.rangeOff-pR) + (pt.aziOff-pA)*(pt.aziOff-pA); ++nc;
        }
        r.perBurstRmse.append(nc > 0 ? std::sqrt(ss / nc) : 0);
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
