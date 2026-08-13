#ifndef PRODUCTDETECTOR_H
#define PRODUCTDETECTOR_H

#include "domain/registration/RegistrationStrategy.h"
#include "domain/SarSensorInfo.h"

// 产品模式识别: 优先 sensor + acquisitionMode 规则;
// burstCount 仅作一致性校验, 不决定模式 (burst 结构 ≠ TOPS 的通用前提)
class ProductDetector {
public:
    // warning 非空时写入一致性/回退警示 (UI 必须展示)
    static ProductMode detect(const SarSensorInfo& si, int burstCount,
                              QString* warning = nullptr);
};

#endif // PRODUCTDETECTOR_H
