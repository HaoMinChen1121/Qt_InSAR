#ifndef IFG_GENERATOR_H
#define IFG_GENERATOR_H

#include "IIfgStep.h"

// Step 1: 复干涉图 + 相干性 + 相位 — 多视 + TOPSAR逐burst干涉 + deburst
class IfgGenerator : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("1. Interferogram Generator"); }
};

#endif // IFG_GENERATOR_H
