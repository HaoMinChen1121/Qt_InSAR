#ifndef IFG_TOPOPHASEREMOVER_H
#define IFG_TOPOPHASEREMOVER_H

#include "IIfgStep.h"

// Step 3: 差分干涉 — DEM地形相位去除
class TopoPhaseRemover : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("3. Topographic Phase Remover"); }
};

#endif // IFG_TOPOPHASEREMOVER_H
