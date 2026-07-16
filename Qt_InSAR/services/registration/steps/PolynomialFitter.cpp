#include "PolynomialFitter.h"
#include "../PipelineContext.h"
#include "algorithms/PolynomialFit.h"
#include <QDebug>

bool PolynomialFitter::execute(PipelineContext& ctx) {
    if (ctx.offsetPoints.size() < 6) {
        ctx.errorMessage = QStringLiteral("Step6: need >=6 points, have %1").arg(ctx.offsetPoints.size());
        return false;
    }
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    if (!fitJointPolynomial(ctx.offsetPoints, w, h, 0, h,
                            ctx.rangePoly, ctx.aziPoly)) {
        ctx.errorMessage = QStringLiteral("Step6: joint polynomial fit failed");
        return false;
    }
    qDebug() << QStringLiteral("[Step6] joint fit: rangeRMSE=%1 (6coeff) aziRMSE=%2 (2coeff)")
        .arg(ctx.rangePoly.rmse, 0, 'f', 3).arg(ctx.aziPoly.rmse, 0, 'f', 3);
    return true;
}
