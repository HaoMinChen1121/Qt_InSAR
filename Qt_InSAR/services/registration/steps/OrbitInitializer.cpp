#include "OrbitInitializer.h"
#include "../PipelineContext.h"
#include "algorithms/OrbitInterpolator.h"

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
    return true;
}
