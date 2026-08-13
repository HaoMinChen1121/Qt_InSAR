#include "PolynomialFitter.h"
#include "../PipelineContext.h"
#include "algorithms/PolynomialFit.h"
#include <QDebug>
#include <cmath>

bool PolynomialFitter::execute(PipelineContext& ctx) {
    if (ctx.offsetPoints.size() < 6) {
        ctx.errorMessage = QStringLiteral("Step7: need >=6 points, have %1").arg(ctx.offsetPoints.size());
        return false;
    }
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;

    // ── 拟合前统计: 定位偏移场是否退化 (诊断用) ──
    {
        double minR=1e9, maxR=-1e9, minA=1e9, maxA=-1e9, sumR=0, sumA=0, sumR2=0, sumA2=0;
        int vc = 0;
        for (const auto& pt : ctx.offsetPoints) {
            if (pt.correlation <= 0) continue;
            minR = qMin(minR, pt.rangeOff); maxR = qMax(maxR, pt.rangeOff);
            minA = qMin(minA, pt.aziOff);   maxA = qMax(maxA, pt.aziOff);
            sumR += pt.rangeOff; sumA += pt.aziOff;
            sumR2 += pt.rangeOff*pt.rangeOff; sumA2 += pt.aziOff*pt.aziOff;
            ++vc;
        }
        QString perBurst;
        int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
        if (N > 1 && L > 0) {
            for (int b = 0; b < N; ++b) {
                double sR = 0, sA = 0; int nc = 0;
                for (const auto& pt : ctx.offsetPoints) {
                    if (pt.correlation <= 0) continue;
                    if (pt.row >= b*L && pt.row < (b+1)*L) { sR += pt.rangeOff; sA += pt.aziOff; ++nc; }
                }
                if (nc > 0)
                    perBurst += QStringLiteral(" b%1:(r=%2,a=%3,n=%4)")
                        .arg(b).arg(sR/nc, 0, 'f', 2).arg(sA/nc, 0, 'f', 2).arg(nc);
            }
        }
        if (vc > 0) {
            double meanR = sumR/vc, meanA = sumA/vc;
            double stdR = vc > 1 ? std::sqrt((sumR2 - vc*meanR*meanR)/(vc-1)) : 0.0;
            double stdA = vc > 1 ? std::sqrt((sumA2 - vc*meanA*meanA)/(vc-1)) : 0.0;
            qDebug().noquote() << QStringLiteral("[Step6] pre-fit: pts=%1 | range mean=%2 std=%3 [%4,%5] | azi mean=%6 std=%7 [%8,%9]%10")
                .arg(vc).arg(meanR, 0, 'f', 3).arg(stdR, 0, 'f', 3)
                .arg(minR, 0, 'f', 2).arg(maxR, 0, 'f', 2)
                .arg(meanA, 0, 'f', 3).arg(stdA, 0, 'f', 3)
                .arg(minA, 0, 'f', 2).arg(maxA, 0, 'f', 2)
                .arg(perBurst);
        }
    }

    int aziOrder = qBound(1, (int)ctx.params->polynomialDegree, 3);
    if (!fitJointPolynomial(ctx.offsetPoints, w, h, 0, h, aziOrder,
                            ctx.rangePoly, ctx.aziPoly)) {
        // 诊断: 法方程奇异与偏移值无关, 只取决于点几何 (row/col),
        // 打印全部信息便于定位退化根因
        qWarning() << "[Step6] joint fit failed: N=" << ctx.offsetPoints.size()
                   << "masterWidth=" << w << "masterHeight=" << h;
        const int kDiagPts = qMin(8, (int)ctx.offsetPoints.size());
        for (int i = 0; i < kDiagPts; ++i) {
            const auto& pt = ctx.offsetPoints[i];
            qWarning() << QStringLiteral("[Step6] pt[%1] row=%2 col=%3 rangeOff=%4 aziOff=%5 corr=%6")
                .arg(i).arg(pt.row).arg(pt.col)
                .arg(pt.rangeOff, 0, 'f', 3).arg(pt.aziOff, 0, 'f', 3)
                .arg(pt.correlation, 0, 'f', 3);
        }
        // 兜底: 降级为一阶模型 (Range[1,r] + Azi[1,a]), 避免整对失败
        if (!fitReducedPolynomial(ctx.offsetPoints, w, h,
                                  ctx.rangePoly, ctx.aziPoly)) {
            ctx.errorMessage = QStringLiteral("Step7: joint polynomial fit failed (reduced fallback also failed)");
            return false;
        }
        qWarning() << QStringLiteral("[Step6] fallback reduced model applied: rangeRMSE=%1 aziRMSE=%2")
            .arg(ctx.rangePoly.rmse, 0, 'f', 3).arg(ctx.aziPoly.rmse, 0, 'f', 3);
    }

    // 剔除残差过大的点，避免 FineCorrelator 的错误精化点污染后续步骤
    {
        QVector<OffsetPoint> kept;
        const double kMaxResidual = 2.0;
        for (const auto& pt : ctx.offsetPoints) {
            double rn = (double)pt.col / w, an = (double)pt.row / h;
            double pR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn + ctx.rangePoly.coeffs[2]*an
                      + ctx.rangePoly.coeffs[3]*rn*an + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
            double pA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an
                      + ctx.aziPoly.coeffs[2]*rn;
            double dr = pt.rangeOff - pR, da = pt.aziOff - pA;
            if (std::sqrt(dr*dr + da*da) <= kMaxResidual)
                kept.append(pt);
        }
        int removed = ctx.offsetPoints.size() - kept.size();
        ctx.offsetPoints = kept;
        qDebug() << QStringLiteral("[Step7] fit done: rangeRMSE=%1 aziRMSE=%2 (%3 pts, removed %4 outliers)")
            .arg(ctx.rangePoly.rmse, 0, 'f', 3).arg(ctx.aziPoly.rmse, 0, 'f', 3)
            .arg(kept.size()).arg(removed);
        if (kept.size() < 6) {
            ctx.errorMessage = QStringLiteral("Step7: too few points after outlier removal");
            return false;
        }
    }
    return true;
}
