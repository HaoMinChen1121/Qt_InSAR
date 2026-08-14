#ifndef IFG_PHASEVISUALIZER_H
#define IFG_PHASEVISUALIZER_H

#include "IIfgStep.h"

// Step: HSV 彩色渲染 (设计 §5.6) — 三通道物理分离:
//   H = (phase+π)/2π   (色相 = 相位, HSV 全循环)
//   S = clip(0.2+0.8·coh, 0, 1)  (饱和度 = 相干性, 低相干灰化)
//   V = log10 幅度 2%/98% 分位数拉伸 (亮度 = 幅度)
// 输出 3-band Byte RGB GeoTIFF (GDAL 直读, 不经过 insartiff)
class PhaseVisualizer : public IIfgStep {
public:
    bool execute(IfgPipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("Phase Visualizer"); }
};

#endif // IFG_PHASEVISUALIZER_H
