#ifndef STEP10_QUALITYEVALUATOR_H
#define STEP10_QUALITYEVALUATOR_H

#include "IRegStep.h"

class QualityEvaluator : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("10. Quality Evaluator"); }
};

#endif
