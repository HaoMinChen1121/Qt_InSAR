#ifndef REG_SINCRESAMPLER_H
#define REG_SINCRESAMPLER_H

#include "IRegStep.h"

// Step 9: Sinc亚像素重采样 — 16点Sinc-Kaiser窗保相插值, GeoTIFF输出
class SincResampler : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("9. Sinc Resampler"); }

private:
    bool resampleNonTopsar(PipelineContext& ctx);
    bool resampleTopsar(PipelineContext& ctx);
};

#endif
