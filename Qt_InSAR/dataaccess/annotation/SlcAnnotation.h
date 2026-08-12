#ifndef SLCANNOTATION_H
#define SLCANNOTATION_H

#include <QString>
#include <QVector>
#include <QDateTime>
#include <QPair>

// ═══════════════════════════════════════════════════════════
//  S1 SLC annotation XML 完整数据结构
//  按 InSAR 流程组织，不照搬 XML 物理结构
// ═══════════════════════════════════════════════════════════

// ── 像对选择 ──
struct ProductIdentity {
    QString   missionId;           // S1A / S1B
    QString   productType;         // SLC
    QString   polarization;        // VV / VH / HH / HV
    QString   mode;                // IW / EW / SM / WV
    QString   swath;               // IW1 / IW2 / IW3
    QDateTime startTime;           // 成像开始 (ISO8601 微秒精度)
    QDateTime stopTime;            // 成像结束
    int       absoluteOrbitNumber = 0;
    QString   missionDataTakeId;
    QString   passDirection;       // Ascending / Descending
};

// ── 轨道/基线 ──
struct OrbitVector {
    QDateTime utcTime;             // 绝对UTC时间
    double    relativeTime = 0.0;  // 相对第一轨秒数 (插值用)
    double    posX = 0.0, posY = 0.0, posZ = 0.0;
    double    velX = 0.0, velY = 0.0, velZ = 0.0;
    QString   frame;               // 坐标系, 通常 "Earth Fixed"
};

// ── TOPS Burst 对齐/ESD ──
struct BurstData {
    QDateTime azimuthTime;
    double    azimuthAnxTime = 0.0; // 相对升交点方位时间 (秒)
    QDateTime sensingTime;
    qint64    byteOffset = 0;       // TIFF 内字节偏移
    int       burstIdRelative = 0;  // 局部 burst 编号
    qint64    burstIdAbsolute = 0;  // 全局唯一 burst ID (跨影像匹配)
    // 注: firstValidSample / lastValidSample (各 ~1494 int) 流式跳过不存储
};

// ── 多普勒质心 (11组完整估计) ──
struct DopplerEstimate {
    QDateTime azimuthTime;
    double    t0 = 0.0;                          // 参考斜距时刻
    QVector<double> geometryDcPoly;               // 3阶 几何法多项式系数
    QVector<double> dataDcPoly;                   // 3阶 数据法多项式系数
    double    dataDcRmsError = 0.0;
    bool      rmsErrorAboveThreshold = false;
    QVector<QPair<double, double>> fineDce;       // 精细DC点: (slantRangeTime, frequency)
};

// ── 方位调频率 ──
struct AzimuthFmRate {
    QDateTime azimuthTime;
    double    t0 = 0.0;
    QVector<double> polynomial;  // 3阶系数
};

// ── 地理定位网格 (粗配准/地理编码关键) ──
struct GeolocationPoint {
    QDateTime azimuthTime;
    double    slantRangeTime = 0.0;
    int       line  = 0;
    int       pixel = 0;
    double    latitude  = 0.0;
    double    longitude = 0.0;
    double    height = 0.0;
    double    incidenceAngle = 0.0;
    double    elevationAngle = 0.0;
};

// ── 处理参数 (主辅一致性检查) ──
struct ProcessingConsistency {
    double  rangeBandwidth     = 0.0;
    double  azimuthBandwidth   = 0.0;
    QString windowType;                  // Hamming 等
    double  windowCoefficient  = 0.0;
    int     numberOfLooks      = 0;
    QString orbitSource;                // Auxiliary / Precise
    QString attitudeSource;             // Downlink / Auxiliary
    bool    srgrApplied        = false;
    bool    thermalNoiseCorrection = false;
};

// ── 姿态数据 (精密轨道插值) ──
struct AttitudeData {
    QDateTime time;
    QString   frame;             // GM2000
    double    q0 = 0.0, q1 = 0.0, q2 = 0.0, q3 = 0.0;   // 四元数
    double    wx = 0.0, wy = 0.0, wz = 0.0;              // 角速度 (rad/s)
    double    roll = 0.0, pitch = 0.0, yaw = 0.0;        // 欧拉角 (deg)
};

// ── 质量信息 (异常数据筛选) ──
struct QualityInfo {
    double productQualityIndex = 0.0;
    // downlinkQuality 关键标志
    bool inputDataMeanOutsideNominalRange = false;
    bool inputDataStDevOutsideNominalRange = false;
    bool downlinkGapsSignificant  = false;
    bool downlinkMissingSignificant = false;
    bool instrumentGapsSignificant   = false;
    bool instrumentMissingSignificant = false;
    // dopplerCentroidQuality
    bool dopplerCentroidUncertain = false;
    // rawDataAnalysisQuality
    bool iBiasSignificant   = false;
    bool qBiasSignificant   = false;
    bool iqGainSignificant  = false;
    bool iqQuadratureSignificant = false;
};

// ── 影像统计 ──
struct ImageStatistics {
    double outputDataMeanRe = 0.0;
    double outputDataMeanIm = 0.0;
    double outputDataStdDevRe = 0.0;
    double outputDataStdDevIm = 0.0;
    bool   outputDataMeanOutsideNominalRangeFlag = false;
};

// ═══════════════════════════════════════════════════
//  完整 annotation 数据容器
// ═══════════════════════════════════════════════════

struct SlcAnnotation {
    ProductIdentity identity;

    // ── 传感器参数 ──
    double radarFrequency       = 0.0;  // 载频 (Hz), 典型 5.405e9
    double rangeSamplingRate    = 0.0;  // 距离采样率 (Hz)
    double azimuthSteeringRate  = 0.0;  // TOPS 方位扫描速率 (deg/s)
    double slantRangeTime       = 0.0;  // 近距斜距时间 (s)
    double rangePixelSpacing    = 0.0;  // 距离向采样间隔 (m)
    double azimuthPixelSpacing  = 0.0;  // 方位向采样间隔 (m)
    double azimuthTimeInterval  = 0.0;  // 方位向时间间隔 (s)
    double azimuthFrequency     = 0.0;  // 有效方位向PRF (Hz)
    double incidenceAngleMidSwath = 0.0; // 中心入射角 (deg)
    double zeroDopMinusAcqTime  = 0.0;  // 零多普勒偏差时间
    int    numberOfSamples      = 0;    // 距离向采样数
    int    numberOfLines        = 0;    // 方位向行数
    int    linesPerBurst        = 0;    // 每 burst 行数
    int    samplesPerBurst      = 0;    // 每 burst 列数
    QString pixelValue;                 // "Complex"
    QString outputPixels;               // "16 bit Signed Integer"

    // ── 轨道 ──
    QVector<OrbitVector> orbitList;      // 16个状态矢量 (含绝对UTC时间)

    // ── Burst ──
    QVector<BurstData> burstList;        // 9个 burst (TOPS 关键)

    // ── 多普勒/调频率 ──
    QVector<DopplerEstimate> dopplerEstimates;  // 11组 + fineDce
    QVector<AzimuthFmRate>   azimuthFmRates;    // 11组 3阶多项式

    // ── 地理定位网格 ──
    QVector<GeolocationPoint> geolocationGrid;  // 210点

    // ── 处理参数 ──
    ProcessingConsistency processing;
    double ellipsoidSemiMajor  = 0.0;
    double ellipsoidSemiMinor  = 0.0;

    // ── 姿态 (精密轨道插值) ──
    QVector<AttitudeData> attitudeList;       // 25组四元数+欧拉角

    // ── 质量/统计 ──
    QualityInfo     quality;       // 异常数据筛选标志
    ImageStatistics stats;         // 复数影像统计
};

#endif // SLCANNOTATION_H
