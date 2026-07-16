#ifndef REG_ESDCORRECTOR_H
#define REG_ESDCORRECTOR_H

#include "IRegStep.h"

// Step 8: TOPS ESD校正 — Burst重叠区干涉相位差→方位常数项修正
class EsdCorrector : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("8. ESD Corrector"); }
};

#endif
