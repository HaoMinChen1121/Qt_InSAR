#ifndef SARSENSORINFO_H
#define SARSENSORINFO_H

#include "SarProductType.h"
#include "OrbitInfo.h"
#include <QString>
#include <QDateTime>

struct DopplerInfo {
    double rangeTime   = 0.0;   // 零多普勒时间 (s)
    double centroid    = 0.0;   // 多普勒质心 (Hz)
    double polynomial0 = 0.0;   // 多普勒多项式系数
    double polynomial1 = 0.0;
    double polynomial2 = 0.0;
};

struct SarSensorInfo {
    // 传感器标识
    QString   sensorType;       // "Sentinel-1", "NISAR", "ALOS-2", ...
    QString   sensorId;         // "S1A", "S1B", ...
    QString   missionId;        // 任务标识
    QString   acquisitionMode;  // "IW", "EW", "SM", "WV" ...
    QString   productId;        // 产品唯一ID

    // 采集信息
    QDateTime acquisitionStart;
    QDateTime acquisitionStop;
    QString   orbitDirection;   // "Ascending" / "Descending"
    int       relativeOrbit = 0;// 相对轨道号
    int       absoluteOrbit = 0;// 绝对轨道号

    // 产品类型
    SarProductType productType = SarProductType::Unknown;
    QString        processingLevel; // "L0", "L1", "L2"

    // 极化
    QStringList polarizations;  // ["VV", "VH"] 等

    // 频率/波长
    double centerFreq  = 5.405e9; // 中心频率 (Hz), 默认 Sentinel-1 C-band
    double wavelength  = 0.0555;  // 波长 (m)

    // 距离向参数
    double rangeSpacing   = 0.0;  // 距离向采样间隔 (m)
    double rangeSamplingRate = 0.0; // 距离向采样率 (Hz)
    double nearRange      = 0.0;  // 近距 (m)
    double farRange       = 0.0;  // 远距 (m)
    int    rangeSamples   = 0;    // 距离向采样数
    double slantRangeRes  = 0.0;  // 斜距分辨率 (m)

    // 方位向参数
    double azimuthSpacing  = 0.0; // 方位向采样间隔 (m)
    double prf             = 0.0; // 脉冲重复频率 (Hz)
    int    azimuthSamples  = 0;   // 方位向采样数
    double azimuthRes      = 0.0; // 方位向分辨率 (m)

    // 入射角
    double incidenceAngleNear  = 0.0; // 近端入射角 (deg)
    double incidenceAngleFar   = 0.0; // 远端入射角 (deg)
    double incidenceAngleMid   = 0.0; // 中心入射角 (deg)

    // 多普勒
    DopplerInfo doppler;

    // 轨道
    QList<OrbitStateVector> orbitStateVectors;

    // 产品路径
    QString originalPath;       // SAFE目录或ZIP路径
    QString manifestPath;       // manifest.safe 路径
    QString annotationDir;      // annotation/ 目录
    QString measurementDir;     // measurement/ 目录
    QString previewPath;        // 预览图路径
};

#endif // SARSENSORINFO_H
