#ifndef COARSECORRELATOR_H
#define COARSECORRELATOR_H

#include "IRegStep.h"

// Step 4: FFT幅度域互相关粗配准
class CoarseCorrelator : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("4. Coarse Correlator"); }
};

#endif
