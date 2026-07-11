#ifndef REGISTRATIONPARAMS_H
#define REGISTRATIONPARAMS_H

#include <QString>
#include <QList>
#include "domain/OrbitInfo.h"
#include "domain/SarSensorInfo.h"

struct RegistrationParams
{
    // 输入 — 显示层路径
    QString   masterPath;
    QString   slavePath;

    // 输入 — SLC 原始复数波段路径（用于读复数数据进行配准）
    QString   masterSlcBandPath;
    QString   slaveSlcBandPath;

    // 输入 — 轨道和传感器元数据（由 ApplicationController 填充）
    QList<OrbitStateVector> masterOrbitVectors;
    QList<OrbitStateVector> slaveOrbitVectors;
    DopplerInfo  masterDoppler;
    DopplerInfo  slaveDoppler;
    double  wavelength = 0.0555;
    double  masterRangeSpacing = 0;
    double  masterAzimuthSpacing = 0;
    double  masterNearRange = 0;
    double  masterPrf = 0;

    // 粗配准
    QString   coarseMethod = "Orbit";      // "Orbit" / "CrossCorrelation"
    int       coarseControlPoints = 64;    // 控制点数

    // 精配准
    QString   fineMethod = "SubPixel";     // "SubPixel" / "Oversample"
    int       fineWindowSize = 32;         // 匹配窗口大小
    int       coarseSearchWindow = 64;     // 互相关搜索窗口半径(像素)
    double    correlationThreshold = 0.3;  // 相关性阈值
    int       polynomialDegree = 2;        // 偏移多项式阶数 (1/2/3)

    // 重采样
    QString   resamplingMethod = "Sinc";   // "Sinc" / "Bilinear" / "Bicubic"
    double    outputResolutionRange = 0;   // 0=保持原始
    double    outputResolutionAzimuth = 0; // 0=保持原始
    int       sincWindowSize = 16;         // Sinc 插值核窗半径
    double    sincBeta = 2.5;              // Kaiser 窗 beta 参数

    // 输出
    QString   outputDir;
    QString   outputPrefix = "registered";

    // 控制
    bool      estimateBaseline = true;     // 是否先估算基线
};

#endif // REGISTRATIONPARAMS_H
