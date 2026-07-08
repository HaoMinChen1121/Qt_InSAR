#ifndef REGISTRATIONPARAMS_H
#define REGISTRATIONPARAMS_H

#include <QString>

struct RegistrationParams
{
    // 输入
    QString   masterPath;
    QString   slavePath;

    // 粗配准
    QString   coarseMethod = "Orbit";      // "Orbit" / "CrossCorrelation"
    int       coarseControlPoints = 64;    // 控制点数

    // 精配准
    QString   fineMethod = "SubPixel";     // "SubPixel" / "Oversample"
    int       fineWindowSize = 32;         // 匹配窗口大小
    double    correlationThreshold = 0.3;  // 相关性阈值

    // 重采样
    QString   resamplingMethod = "Sinc";   // "Sinc" / "Bilinear" / "Bicubic"
    double    outputResolutionRange = 0;   // 0=保持原始
    double    outputResolutionAzimuth = 0; // 0=保持原始

    // 输出
    QString   outputDir;
    QString   outputPrefix = "registered";
};

#endif // REGISTRATIONPARAMS_H
