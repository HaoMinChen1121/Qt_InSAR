#ifndef REGISTRATIONPARAMS_H
#define REGISTRATIONPARAMS_H

#include <QString>
#include <QList>
#include "domain/OrbitInfo.h"
#include "domain/SarSensorInfo.h"

enum class RegRoute {
    Route1_OrbitFFT  = 0,   // 快速: 轨道几何 + 小窗口FFT幅度相关 → 重采样
    Route2_NCC_FFTW  = 1,   // 稳健: NCC搜索 → FFTW3复数相位相关 → ESD → 重采样
    Route3_FFT_FFTW  = 2    // 标准: FFT幅度域 → 多项式拟合 → FFTW3相位相关 → ESD → 重采样 (推荐)
};

struct RegistrationParams
{
    // ── 配准路线 ──
    RegRoute  route = RegRoute::Route3_FFT_FFTW;  // 默认标准路线

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

    // ── 粗配准 ──
    QString   coarseMethod = "Orbit";       // "Orbit" / "CrossCorrelation" / "FFT"
    int       coarseControlPoints = 64;
    int       coarseSearchWindow = 64;
    int       coarseWindowSize  = 32;       // NCC窗口
    int       offsetPerBurst    = 64;       // 每burst采样点数
    // ── 精配准 ──
    QString   fineMethod = "SubPixel";      // "SubPixel" / "Oversample"
    int       fineWindowSize = 32;
    double    correlationThreshold = 0.3;
    int       polynomialDegree = 2;
    // ── FFTW3 ──
    bool      enableFineFFT   = false;     // FFTW3复数域精配准
    int       fineFFTWindow   = 256;       // 精配准FFT窗口

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
    bool      enableEsd = true;      // TOPSAR ESD 方位向精化
    int       esdOverlapLines = 0;    // ESD重叠行数 (0=自动取 L/10)
    double    deltaFdoppler = 5000.0; // TOPSAR burst间多普勒质心差 (Hz)

    // ── 兼容旧UI路径字段 ──
    QString   masterPath;   // 用于对话框显示
    QString   slavePath;
};

#endif // REGISTRATIONPARAMS_H
