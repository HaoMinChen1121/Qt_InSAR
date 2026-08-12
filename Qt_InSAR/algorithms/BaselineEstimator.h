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
// masterTime: 主影像采集时间 (用于插值中点)
// nearRange / farRange: 近/远距 (用于LOS方向估算)
BaselineResult computeBaseline(
    const QList<OrbitStateVector>& masterOrbit,
    const QList<OrbitStateVector>& slaveOrbit,
    const QDateTime& masterTime,
    double nearRange,
    double farRange);

} // namespace sar

#endif // BASELINEESTIMATOR_H
