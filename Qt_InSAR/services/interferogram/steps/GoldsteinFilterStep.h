#ifndef IFG_GOLDSTEINFILTERSTEP_H
#define IFG_GOLDSTEINFILTERSTEP_H

#include "IIfgStep.h"

// Step 4: Goldstein 自适应滤波 (作用于去地形/去平地后的复数干涉图;
//         输出为滤波后复数产品, 供相干性估计与下游使用)
class GoldsteinFilterStep : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("4. Goldstein Filter"); }
};

#endif // IFG_GOLDSTEINFILTERSTEP_H
