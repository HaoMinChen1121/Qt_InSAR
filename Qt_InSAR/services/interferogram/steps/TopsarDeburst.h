#ifndef IFG_TOPSARDEBURST_H
#define IFG_TOPSARDEBURST_H

#include "IIfgStep.h"

#include <QDateTime>
#include <QVector>

// Step 2: TOPSAR Deburst — 方位时间校正 (残余方位斜坡去除, 符号 A/B 验证后固化)
//         + 相邻 burst 重叠裁剪 + 顺序拼接
class TopsarDeburst : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("2. TOPSAR Deburst"); }
};

// 相邻 burst 重叠行数 (deburst 输出行→方位时间映射与 TopoPhaseRemover 共用)
void computeBurstDiscard(int N, int linesPerBurst, double prf,
                         const QVector<QDateTime>& burstTimes,
                         QVector<int>& discardTop, QVector<int>& discardBottom);

#endif // IFG_TOPSARDEBURST_H
