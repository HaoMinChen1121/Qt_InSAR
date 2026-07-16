#ifndef REG_OFFSETEXTRACTOR_H
#define REG_OFFSETEXTRACTOR_H

#include "IRegStep.h"

// Step 5: Offset点提取 — 逐Burst提取(x,y,dr,da,corr)观测点
class OffsetExtractor : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("5. Offset Extractor"); }
};

#endif
