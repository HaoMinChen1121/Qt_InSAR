#include "OrbitInitializer.h"
#include "../PipelineContext.h"
#include "algorithms/OrbitInterpolator.h"
#include <QDebug>
#include <QStringList>

bool OrbitInitializer::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int N = ctx.data.burstCount;
    int L = ctx.data.linesPerBurst;
    int colMid = ctx.data.masterWidth / 2;

    if (ctx.isTopsar) {
        ctx.initialOffsets.resize(N);
        for (int b = 0; b < N; ++b) {
            int centerRow = b * L + L / 2;
            computeOrbitOffset(p.masterOrbitVectors, p.slaveOrbitVectors,
                p.masterNearRange, p.masterRangeSpacing,
                p.masterAzimuthSpacing, p.masterPrf,
                centerRow, colMid,
                ctx.initialOffsets[b].rangeOff, ctx.initialOffsets[b].aziOff);
            ctx.initialOffsets[b].burstIndex = b;
        }
    } else {
        ctx.initialOffsets.resize(1);
        int centerRow = ctx.data.masterHeight / 2;
        computeOrbitOffset(p.masterOrbitVectors, p.slaveOrbitVectors,
            p.masterNearRange, p.masterRangeSpacing,
            p.masterAzimuthSpacing, p.masterPrf,
            centerRow, colMid,
            ctx.initialOffsets[0].rangeOff, ctx.initialOffsets[0].aziOff);
    }

    // 诊断: 逐 burst 初始偏移 (与 pre-fit 统计对比可定位错位来源)
    {
        QStringList inits;
        for (const auto& io : ctx.initialOffsets)
            inits << QStringLiteral("b%1:(r=%2,a=%3)")
                .arg(io.burstIndex)
                .arg(io.rangeOff, 0, 'f', 2).arg(io.aziOff, 0, 'f', 2);
        qDebug().noquote() << "[Step3] orbit init offsets:" << inits.join(QStringLiteral(" "));
    }
    return true;
}
