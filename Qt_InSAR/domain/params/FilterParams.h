#ifndef FILTERPARAMS_H
#define FILTERPARAMS_H

#include <QString>

struct FilterParams
{
    // 滤波方法
    QString   method = "Goldstein";        // "Goldstein" / "Baran"

    // Goldstein & Werner
    double    goldsteinAlpha = 0.5;         // 滤波强度 (0~1)
    int       goldsteinWindowSize = 32;     // 窗口大小
    int       goldsteinPatchSize = 64;      // 重叠分块大小

    // Baran et al.
    double    baranAlpha = 0.5;             // 自适应滤波强度
    int       baranWindowSize = 32;
    int       baranIterations = 3;          // 迭代次数

    // 通用
    bool      useCoherenceMask = false;     // 相干掩膜
    double    coherenceThreshold = 0.3;     // 相干阈值
    QString   maskFilePath;
};

#endif // FILTERPARAMS_H
