#ifndef REG_POLYNOMIALFITTER_H
#define REG_POLYNOMIALFITTER_H

#include "IRegStep.h"

// Step 6: 联合多项式拟合 — 全部观测点最小二乘 Range6系数+Azi2系数
class PolynomialFitter : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("6. Polynomial Fitter"); }
};

#endif
