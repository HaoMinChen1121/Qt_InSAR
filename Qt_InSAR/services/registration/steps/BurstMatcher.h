#ifndef REG_BURSTMATCHER_H
#define REG_BURSTMATCHER_H

#include "IRegStep.h"

// Step 2: Burst匹配 — 相对azimuthTime匹配主辅burst对应关系
class BurstMatcher : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("2. Burst Matcher"); }
};

#endif
