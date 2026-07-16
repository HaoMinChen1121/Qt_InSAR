#ifndef REG_ORBITINITIALIZER_H
#define REG_ORBITINITIALIZER_H

#include "IRegStep.h"

// Step 3: 初始几何配准 — 轨道状态向量三次样条插值, 估算初始偏移
class OrbitInitializer : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("3. Orbit Initializer"); }
};

#endif
