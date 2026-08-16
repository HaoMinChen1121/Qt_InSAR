#ifndef IFG_COHERENCEESTIMATOR_H
#define IFG_COHERENCEESTIMATOR_H

#include "IIfgStep.h"

// Step 5: 相干性估计 — 5×5 多视网格滑窗
//   默认在滤波后产品上估计并覆盖 merge/S1_POL_coh.tif (与 ASF corr.tif 同口径);
//   outputPathOverride 非空时写指定路径 (raw coh 诊断口径, 滤波前产品)
class CoherenceEstimator : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("5. Coherence Estimator"); }
    QString outputPathOverride;   // 非空 = 输出到此路径 (raw coh 诊断)
};

#endif // IFG_COHERENCEESTIMATOR_H
