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
        const auto& mAnx = ctx.masterBand->burstAzimuthAnxTimes;
        const auto& sAnx = ctx.slaveBand->burstAzimuthAnxTimes;
        const double prf = p.masterPrf > 0 ? p.masterPrf
            : ctx.data.masterAzimuthFrequency;
        for (int b = 0; b < N; ++b) {
            double rangeOff = 0, aziOff = 0;
            // 方位偏移 = 主辅 burst ANX 时间差 × PRF (精确时序真值)
            // 轨道插值不可靠: 两产品轨道矢量 relativeTime 各自从自身首矢量起算,
            // 时间帧零点不对齐 → 实测产生 238 行伪偏移 (真值 ~1.4 行);
            // ANX 时间由 ESA 轨道确定, 与 BurstMatcher 同源, 无帧对齐问题
            if (prf > 0 && mAnx.size() > b && sAnx.size() > b) {
                int j = b;
                if (ctx.burstPairs.size() == N
                    && ctx.burstPairs[b].isValid
                    && ctx.burstPairs[b].slaveBurstIdx >= 0
                    && ctx.burstPairs[b].slaveBurstIdx < sAnx.size())
                    j = ctx.burstPairs[b].slaveBurstIdx;
                aziOff = (mAnx[b] - sAnx[j]) * prf;
            } else {
                int centerRow = b * L + L / 2;
                computeOrbitOffset(p.masterOrbitVectors, p.slaveOrbitVectors,
                    p.masterNearRange, p.masterRangeSpacing,
                    p.masterAzimuthSpacing, p.masterPrf,
                    centerRow, colMid,
                    rangeOff, aziOff);
            }
            ctx.initialOffsets[b].rangeOff = rangeOff;
            ctx.initialOffsets[b].aziOff = aziOff;
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
