#include "PolynomialFitter.h"
#include "../PipelineContext.h"
#include "algorithms/PolynomialFit.h"
#include <QDebug>

bool PolynomialFitter::execute(PipelineContext& ctx) {
    if (ctx.offsetPoints.size() < 6) {
        ctx.errorMessage = QStringLiteral("Step7: need >=6 points, have %1").arg(ctx.offsetPoints.size());
        return false;
    }
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    if (!fitJointPolynomial(ctx.offsetPoints, w, h, 0, h,
                            ctx.rangePoly, ctx.aziPoly)) {
        ctx.errorMessage = QStringLiteral("Step7: joint polynomial fit failed");
        return false;
    }

    // 剔除残差过大的点，避免 FineCorrelator 的错误精化点污染后续步骤
    {
        QVector<OffsetPoint> kept;
        const double kMaxResidual = 2.0;
        for (const auto& pt : ctx.offsetPoints) {
            double rn = (double)pt.col / w, an = (double)pt.row / h;
            double pR = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rn + ctx.rangePoly.coeffs[2]*an
                      + ctx.rangePoly.coeffs[3]*rn*an + ctx.rangePoly.coeffs[4]*rn*rn + ctx.rangePoly.coeffs[5]*an*an;
            double pA = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1]*an;
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
