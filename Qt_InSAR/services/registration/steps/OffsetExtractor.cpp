#include "OffsetExtractor.h"
#include "../PipelineContext.h"
#include <QDebug>

bool OffsetExtractor::execute(PipelineContext& ctx) {
    // Fast 等级无精配准，不需要过滤
    if (!ctx.strategy || !ctx.strategy->useFine)
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
