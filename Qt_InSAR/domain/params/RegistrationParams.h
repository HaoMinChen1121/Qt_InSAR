#ifndef REGISTRATIONPARAMS_H
#define REGISTRATIONPARAMS_H

#include <QString>
#include <QList>
#include <optional>
#include "domain/OrbitInfo.h"
#include "domain/SarSensorInfo.h"
#include "domain/registration/ProcessingLevel.h"
#include "domain/registration/ProductMode.h"
#include "domain/registration/CorrelationMethod.h"

// 每处理等级独立的一套用户参数 (等级切换不互相覆盖)
struct RegistrationProfileParams {
    int    coarseWindowSize = 256;
    int    coarseSearchWindow = 64;   // NCC 搜索半径
    int    fineWindowSize = 256;
    int    offsetPerBurst = 8;
    double correlationThreshold = 0.3;
    int    polynomialDegree = 2;      // 方位模型阶数: 1=[1] 2=[1,a] 3=[1,a,r]
};

struct RegistrationParams
{
    // ── 策略 ──
    ProcessingLevel level = ProcessingLevel::Standard;
    ProductMode  productMode = ProductMode::GENERIC;   // 控制器选产品时填充 (UI 显示用)
    std::optional<CorrelationMethod> coarseCorrOverride;  // 用户覆盖粗配准引擎 (无值=策略默认)
    RegistrationProfileParams profiles[3];   // Fast/Standard/High 各一份

    // ── 当前生效的扁平参数 (由 profiles[level] 同步, 管线直接读取) ──
    int       coarseSearchWindow = 64;
    int       coarseWindowSize  = 256;
    int       offsetPerBurst    = 8;        // 每burst采样点数
    int       fineWindowSize = 256;         // 精配准窗口
    double    correlationThreshold = 0.3;
    int       polynomialDegree = 2;

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
    double  wavelength = 0.0;
    double  baselinePerp = 0.0;      // 垂直基线(m), 由轨道向量计算
    double  baselinePar  = 0.0;      // 平行基线(m), 由轨道向量计算
    double  masterRangeSpacing = 0;
    double  masterAzimuthSpacing = 0;
    double  masterNearRange = 0;
    double  masterPrf = 0;

    // ── 重采样 ──
    QString   resamplingMethod = "Sinc";    // "Sinc" / "Bilinear" / "Bicubic"
    int       sincWindowSize = 8;   // 33→17 taps, 计算量减半
    double    sincBeta = 2.5;

    // ── 输出 ──
    QString   outputDir;
    QString   outputPrefix = "registered";
    bool      estimateBaseline = true;
    int       esdOverlapLines = 0;    // ESD重叠行数 (0=自动取 L/10)
    double    deltaFdoppler = 0.0;   // TOPSAR burst间多普勒质心差 (Hz), 由产品XML填充

    // ── 兼容旧UI路径字段 ──
    QString   masterPath;   // 用于对话框显示
    QString   slavePath;
};

#endif // REGISTRATIONPARAMS_H
