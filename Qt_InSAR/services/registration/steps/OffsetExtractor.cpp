#include "OffsetExtractor.h"
#include "../PipelineContext.h"
#include <QDebug>

bool OffsetExtractor::execute(PipelineContext& ctx) {
    // 路线1无多项式拟合，不需要过滤
    if (ctx.params->route == RegRoute::Route1_OrbitFFT)
        return true;

    const auto& p = *ctx.params;
    QVector<OffsetPoint> filtered;
    for (const auto& pt : ctx.offsetPoints)
        if (pt.correlation >= p.correlationThreshold)
            filtered.append(pt);

    qDebug() << QStringLiteral("[Step5] %1/%2 offset points passed threshold %3")
        .arg(filtered.size()).arg(ctx.offsetPoints.size()).arg(p.correlationThreshold, 0, 'f', 2);

    if (filtered.size() < 6) {
        ctx.errorMessage = QStringLiteral("Step5: insufficient valid offsets (%1)").arg(filtered.size());
        return false;
    }
    ctx.offsetPoints = filtered;
    return true;
}
