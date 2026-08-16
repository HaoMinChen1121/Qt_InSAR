#ifndef IFG_COHERENCEESTIMATOR_H
#define IFG_COHERENCEESTIMATOR_H

#include "IIfgStep.h"

// Step 5: 相干性估计 — 在滤波后的去地形/去平地产品上估计
//   (5×5 多视网格滑窗, 对齐 ASF/GAMMA 的相干产品位置;
//    覆盖 merge/S1_POL_coh.tif — 与 ASF corr.tif 同口径)
class CoherenceEstimator : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("5. Coherence Estimator"); }
};

#endif // IFG_COHERENCEESTIMATOR_H
