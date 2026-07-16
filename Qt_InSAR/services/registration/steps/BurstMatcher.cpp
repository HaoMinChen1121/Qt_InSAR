#include "BurstMatcher.h"
#include "../PipelineContext.h"
#include <QDebug>
#include <cmath>

bool BurstMatcher::execute(PipelineContext& ctx) {
    if (!ctx.isTopsar) return true;
    int N = ctx.data.burstCount;
    const auto& mT = ctx.data.masterBurstTimes;
    const auto& sT = ctx.data.slaveBurstTimes;

    // 无burst时间数据时降级为顺序匹配
    if (mT.size() < N || sT.size() < N) {
        ctx.burstPairs.resize(N);
        for (int i = 0; i < N; ++i)
            ctx.burstPairs[i] = {i, i, 0.0, true};
        return true;
    }

    // 转换为相对时间 (秒, 从第一个burst起算)
    QVector<double> mRel(N), sRel(N);
    bool allValid = true;
    for (int i = 0; i < N; ++i) {
        if (!mT[i].isValid() || !sT[i].isValid()) { allValid = false; break; }
        mRel[i] = mT[0].msecsTo(mT[i]) / 1000.0;
        sRel[i] = sT[0].msecsTo(sT[i]) / 1000.0;
    }
    if (!allValid) {
        // 降级: 顺序匹配
        ctx.burstPairs.resize(N);
        for (int i = 0; i < N; ++i)
            ctx.burstPairs[i] = {i, i, 0.0, true};
        return true;
    }

    const double kMaxDelta = 0.5; // 同轨burst相对时序差 <0.5s
    ctx.burstPairs.resize(N);
    int matched = 0;
    for (int i = 0; i < N; ++i) {
        double bestDelta = 1e12; int bestJ = 0;
        for (int j = 0; j < N; ++j) {
            double d = std::abs(mRel[i] - sRel[j]);
            if (d < bestDelta) { bestDelta = d; bestJ = j; }
        }
        ctx.burstPairs[i] = {i, bestJ, bestDelta, bestDelta < kMaxDelta};
        if (bestDelta < kMaxDelta) ++matched;
    }
    qDebug() << QStringLiteral("[Step2] matched %1/%2 bursts by relative time").arg(matched).arg(N);
    if (matched < N) {
        qDebug() << "[Step2] fallback to sequential matching";
        for (int i = 0; i < N; ++i)
            if (!ctx.burstPairs[i].isValid)
                ctx.burstPairs[i] = {i, i, -1.0, true};
    }
    return true;
}
