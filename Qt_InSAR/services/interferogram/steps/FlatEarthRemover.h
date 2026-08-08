#ifndef IFG_FLATEARTHREMOVER_H
#define IFG_FLATEARTHREMOVER_H

#include "IIfgStep.h"

// Step 2: 平地相位去除 — 椭球面近似, 复数旋转去除平地条纹
class FlatEarthRemover : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("2. Flat-Earth Remover"); }
};

#endif // IFG_FLATEARTHREMOVER_H
