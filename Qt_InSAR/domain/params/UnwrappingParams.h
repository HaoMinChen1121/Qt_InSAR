#ifndef UNWRAPPINGPARAMS_H
#define UNWRAPPINGPARAMS_H

#include <QString>

struct UnwrappingParams
{
    // 解缠方法
    QString   method = "BranchCut";         // "BranchCut" / "LeastSquares"
    double    coherenceThreshold = 0.3;     // 相干阈值
    QString   maskPath;                     // 掩膜文件 (可选)
    int       minRegionSize = 100;          // 最小区域 (像素)

    // 枝切法
    int       branchCutMaxResidues = 500;   // 最大残差点数
    bool      useDelaunayTriangulation = true;

    // 最小二乘法
    bool      useWeightedLeastSquares = true;
    int       maxIterations = 1000;
    double    convergenceTolerance = 1e-4;

    // 注: 相位→高程/形变转换参数已移交 DeformationParams (Phase 0 设计决定)
};

#endif // UNWRAPPINGPARAMS_H
