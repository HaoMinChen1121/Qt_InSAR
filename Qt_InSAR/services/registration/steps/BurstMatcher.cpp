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

    bool allValid = true;
    for (int i = 0; i < N; ++i) {
        if (!mT[i].isValid()) { allValid = false; break; }
    }
    for (int j = 0; j < sT.size(); ++j) {
        if (!sT[j].isValid()) { allValid = false; break; }
    }
    if (!allValid) {
        // 降级: 顺序匹配
        ctx.burstPairs.resize(N);
        for (int i = 0; i < N; ++i)
            ctx.burstPairs[i] = {i, i, 0.0, true};
        return true;
    }

    // ── 匹配链 ──
    // 1) 绝对 burst ID (annotation <burstId absolute=...>, 跨影像全局唯一)
    // 2) ANX 时间 (相对升交点方位时间, 消除跨天时间基线) + 整 burst 平移搜索
    // 3) 相对时间平移搜索 (无 ANX 数据时)
    // 4) 顺序降级
    const double kMaxDelta = 0.5;
    const auto& mIds = ctx.masterBand->burstAbsoluteIds;
    const auto& sIds = ctx.slaveBand->burstAbsoluteIds;
    const auto& mAnx = ctx.masterBand->burstAzimuthAnxTimes;
    const auto& sAnx = ctx.slaveBand->burstAzimuthAnxTimes;
    const int sN = sT.size();

    ctx.burstPairs.resize(N);
    int matched = 0;
    QString method;

    if (mIds.size() >= N && !sIds.isEmpty()) {
        method = QStringLiteral("absolute burst ID");
        for (int i = 0; i < N; ++i) {
            int bestJ = -1;
            for (int j = 0; j < sIds.size(); ++j)
                if (sIds[j] == mIds[i]) { bestJ = j; break; }
            ctx.burstPairs[i] = {i, bestJ, 0.0, bestJ >= 0};
            if (bestJ >= 0) ++matched;
        }
    } else if (mAnx.size() >= N && sAnx.size() >= 2) {
        method = QStringLiteral("ANX time + shift search");
        double T = mAnx[1] - mAnx[0];
        if (std::abs(T) < 1e-6) T = 3.0;
        // 整 burst 平移: 两条时间线的 ANX 差 / burst 周期
        int shift = static_cast<int>(std::lround((mAnx[0] - sAnx[0]) / T));
        int dMin = -shift, dMax = sN - N - shift;
        int bestShift = 0; double bestVar = 1e18, bestMean = 0;
        for (int d = dMin; d <= dMax; ++d) {
            double sum = 0, sumSq = 0;
            for (int i = 0; i < N; ++i) {
                double diff = mAnx[i] - sAnx[i + shift + d];
                sum += diff; sumSq += diff * diff;
            }
            double var = sumSq / N - (sum / N) * (sum / N);
            if (var < bestVar) { bestVar = var; bestMean = sum / N; bestShift = shift + d; }
        }
        for (int i = 0; i < N; ++i) {
            int j = i + bestShift;
            if (j < 0 || j >= sN) { ctx.burstPairs[i] = {i, j, 1e9, false}; continue; }
            double delta = std::abs(mAnx[i] - sAnx[j] - bestMean);
            ctx.burstPairs[i] = {i, j, delta, delta < kMaxDelta};
            if (delta < kMaxDelta) ++matched;
        }
    } else if (sN >= N) {
        method = QStringLiteral("relative time + shift search");
        QVector<double> mRel(N), sRel(sN);
        for (int i = 0; i < N; ++i) mRel[i] = mT[0].msecsTo(mT[i]) / 1000.0;
        for (int j = 0; j < sN; ++j) sRel[j] = sT[0].msecsTo(sT[j]) / 1000.0;
        int bestShift = 0; double bestCost = 1e18;
        for (int d = 0; d <= sN - N; ++d) {
            double sum = 0, sumSq = 0;
            for (int i = 0; i < N; ++i) {
                double diff = mRel[i] - sRel[i + d];
                sum += diff; sumSq += diff * diff;
            }
            double var = sumSq / N - (sum / N) * (sum / N);
            double cost = var + std::abs(sum / N) * 1e-6;  // 优先接近零均值
            if (cost < bestCost) { bestCost = cost; bestShift = d; }
        }
        for (int i = 0; i < N; ++i) {
            int j = i + bestShift;
            double delta = std::abs(mRel[i] - sRel[j]);
            ctx.burstPairs[i] = {i, j, delta, delta < kMaxDelta};
            if (delta < kMaxDelta) ++matched;
        }
    } else {
        method = QStringLiteral("sequential (slave burst count < master)");
    }

    qDebug() << QStringLiteral("[Step2] matched %1/%2 bursts by %3").arg(matched).arg(N).arg(method);
    if (matched < N) {
        qDebug() << "[Step2] fallback to sequential matching";
        for (int i = 0; i < N; ++i)
            if (!ctx.burstPairs[i].isValid)
                ctx.burstPairs[i] = {i, i, -1.0, true};
    }
    return true;
}
