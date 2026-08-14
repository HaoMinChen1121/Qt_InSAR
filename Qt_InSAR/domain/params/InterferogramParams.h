#ifndef INTERFEROGRAMPARAMS_H
#define INTERFEROGRAMPARAMS_H

#include <QString>

struct InterferogramParams
{
    // 输入 — QSAR产品路径
    QString   masterQsarPath;             // 主影像QSAR路径 (.qsar)
    QString   slaveQsarPath;              // 辅影像QSAR路径 (registered.qsar)
    QString   masterProductDisplay;       // 主产品显示名
    QString   slaveProductDisplay;        // 辅产品显示名

    // 干涉图生成
    int       rangeLooks = 4;            // 距离向多视比 (4→16x缩小)
    int       azimuthLooks = 4;           // 方位向多视比
    QString   outputType = "Complex";     // "Complex" / "Phase" / "Coherence"
    bool      spectralFilter = true;      // 频谱偏移滤波
    bool      enableAzimuthRampCorrection = true;  // deburst 方位时间校正开关
    int       azimuthRampCorrectionSign  = -1;     // 开发期诊断: {0=不校正, ±1=符号};
                                                   // Stage 1 A/B/C 验证后固化, 不进 UI,
                                                   // 可被环境变量 INSAR_DEBURST_SIGN 覆盖
    bool      phaseAlign              = true;      // IW 拼接相位一致性对齐 (常数+线性)
    bool      legacyPerIwOutputs      = false;     // 兼容输出开关: 从合并产品切片生成逐 IW flat/diff

    // 去平地效应
    QString   referenceSource = "Orbit";  // "Orbit" / "Ellipsoid"
    QString   orbitFilePath;              // 轨道文件路径

    // 差分干涉
    bool      differential = false;
    QString   demPath;                    // DEM文件路径
    QString   displacementDirection = "LOS"; // "LOS" / "Vertical"
    bool      atmosphericCorrection = false;

    // 输出
    QString   outputDir;
    QString   outputPrefix = "interferogram";
    double    incidenceAngle = 0.0;       // 入射角(度), 从主产品XML获取
    double    wavelength = 0.0;           // 波长(m), 从主产品XML获取
    double    nearRange = 0.0;             // 近距(m), 从主产品XML获取
    double    rangeSpacing = 0.0;          // 距离向采样间隔(m), 从主产品XML获取
    double    prf = 0.0;                   // PRF(Hz), 从主产品XML获取
    double    baselinePerp = 0.0;          // 垂直基线(m), 从轨道向量计算
    double    baselinePar  = 0.0;          // 平行基线(m), 从轨道向量计算
    bool      enableFlatEarth = true;
    bool      enableDifferential = false;
};

#endif // INTERFEROGRAMPARAMS_H
