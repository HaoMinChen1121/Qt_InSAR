#ifndef REG_FINECORRELATOR_H
#define REG_FINECORRELATOR_H

#include "IRegStep.h"

// Step 7: 精配准 — FFTW3幅度域互相关, 亚像素峰值定位
class FineCorrelator : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("7. Fine Correlator"); }
};

#endif
