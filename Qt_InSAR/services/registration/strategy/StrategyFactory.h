#ifndef STRATEGYFACTORY_H
#define STRATEGYFACTORY_H

#include "domain/registration/RegistrationStrategy.h"

// 二维策略矩阵: ProductMode × ProcessingLevel → RegistrationStrategy
// 不变式: ProductMode 决定物理模型 (offsetModel/azimuthModel/azimuthCorrection);
//         ProcessingLevel 只调整参数预设 (点数/阶数, 窗口由 UI 层叠加),
//         不改变 TOPS 方位几何模型 (OrbitGeometry 为硬结论)
class StrategyFactory {
public:
    static RegistrationStrategy create(ProductMode mode, ProcessingLevel level);
};

#endif // STRATEGYFACTORY_H
