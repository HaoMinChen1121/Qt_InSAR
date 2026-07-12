#ifndef REGISTRATIONPARAMS_H
#define REGISTRATIONPARAMS_H

#include <QString>
#include <QList>
#include "domain/OrbitInfo.h"
#include "domain/SarSensorInfo.h"

struct RegistrationParams
{
    // ── 输入 — 产品 (SAFE/zip 根路径) ──
    QString   masterProductPath;   // 主产品路径
    QString   slaveProductPath;    // 辅产品路径
    QString   masterDisplayName;   // 主产品显示名
    QString   slaveDisplayName;    // 辅产品显示名

    // ── 轨道和传感器元数据 ──
    QList<OrbitStateVector> masterOrbitVectors;
    QList<OrbitStateVector> slaveOrbitVectors;
    DopplerInfo  masterDoppler;
    DopplerInfo  slaveDoppler;
    double  wavelength = 0.0555;
    double  masterRangeSpacing = 0;
    double  masterAzimuthSpacing = 0;
    double  masterNearRange = 0;
    double  masterPrf = 0;

    // ── 配准参数 ──
    QString   coarseMethod = "Orbit";       // "Orbit" / "CrossCorrelation"
    int       coarseControlPoints = 64;
    QString   fineMethod = "SubPixel";      // "SubPixel" / "Oversample"
    int       fineWindowSize = 32;
    int       coarseSearchWindow = 64;
    double    correlationThreshold = 0.3;
    int       polynomialDegree = 2;

    // ── 重采样 ──
    QString   resamplingMethod = "Bilinear";// "Sinc" / "Bilinear" / "Bicubic"
    double    outputResolutionRange = 0;    // 0=保持原始
    double    outputResolutionAzimuth = 0;
    int       sincWindowSize = 16;
    double    sincBeta = 2.5;

    // ── 输出 ──
    QString   outputDir;
    QString   outputPrefix = "registered";
    bool      estimateBaseline = true;

    // ── 兼容旧UI路径字段 ──
    QString   masterPath;   // 用于对话框显示
    QString   slavePath;
};

#endif // REGISTRATIONPARAMS_H
