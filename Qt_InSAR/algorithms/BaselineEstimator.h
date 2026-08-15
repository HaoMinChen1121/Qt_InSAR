#ifndef BASELINEESTIMATOR_H
#define BASELINEESTIMATOR_H

#include "domain/OrbitInfo.h"
#include <QList>
#include <QDateTime>

namespace sar {

struct BaselineResult {
    double perpBaseline  = 0.0;  // vertical perpendicular baseline (m)
    double parBaseline   = 0.0;  // parallel baseline along LOS (m)
    double temporalBaseline = 0.0; // temporal baseline (days)
    bool   valid = false;
};

// 从主/辅影像轨道状态向量计算干涉基线
// masterOrbit / slaveOrbit: 轨道状态向量
// masterTime: 主影像采集开始时间 (绝对UTC, 用于时间基线)
// slaveTime: 辅影像采集开始时间 (绝对UTC, 用于时间基线)
// nearRange / farRange: 近/远距 (用于LOS方向估算)
// 位置插值: 两轨道各自插值到自身场景中点 (同一沿轨相位) —
// 主辅相隔 12 天, 辅轨道跨度不覆盖主景中点绝对时刻, 不能用同一绝对时间插值
BaselineResult computeBaseline(
    const QList<OrbitStateVector>& masterOrbit,
    const QList<OrbitStateVector>& slaveOrbit,
    const QDateTime& masterTime,
    const QDateTime& slaveTime,
    double nearRange,
    double farRange);

} // namespace sar

#endif // BASELINEESTIMATOR_H
