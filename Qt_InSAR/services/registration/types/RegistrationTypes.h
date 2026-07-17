#ifndef REGISTRATIONTYPES_H
#define REGISTRATIONTYPES_H

#include <QVector>
#include <QString>
#include <QDateTime>
#include <complex>

// ── Offset观测点 ──
struct OffsetPoint {
    int    row = 0;
    int    col = 0;
    int    origIdx = -1;       // 原始索引 (并行处理时用于写回)
    double rangeOff = 0.0;     // 距离向偏移 (pixel)
    double aziOff   = 0.0;     // 方位向偏移 (pixel)
    double correlation = 0.0;  // NCC系数
};

// ── 距离向多项式: Δr = a0 + a1·r + a2·a + a3·r·a + a4·r² + a5·a² ──
struct RangePolynomial {
    double coeffs[6] = {};
    double rmse = 0.0;
};

// ── 方位向多项式: Δa = b0 + b1·a (低阶, TOPS对高阶敏感) ──
struct AzimuthPolynomial {
    double coeffs[2] = {};
    double rmse = 0.0;
};

// ── Burst匹配对 ──
struct BurstMatchPair {
    int    masterBurstIdx = -1;
    int    slaveBurstIdx  = -1;
    double timeDeltaSec   = 0.0;
    bool   isValid        = false;
};

// ── 逐Burst偏移 ──
struct BurstOffset {
    int    burstIndex = -1;
    double rangeOff   = 0.0;
    double aziOff     = 0.0;
};

// ── 逐Burst配准结果 ──
struct BurstRegResult {
    int                          burstIndex = -1;
    RangePolynomial              rangePoly;
    AzimuthPolynomial            aziPoly;
    QVector<std::complex<float>> data;     // 重采样后数据
    double                       correlation = 0.0;
};

// ── SLC数据包 ──
struct SlcDataBundle {
    int masterWidth   = 0;
    int masterHeight  = 0;
    int slaveWidth    = 0;
    int slaveHeight   = 0;
    int burstCount    = 0;
    int linesPerBurst = 0;
    QVector<int>       burstStartLines;
    QVector<QDateTime> masterBurstTimes;
    QVector<QDateTime> slaveBurstTimes;
    double  slaveAzimuthFmRate = 0.0;       // 辅影像方位向调频率 (Hz/s, TOPS deramp)
    double  slaveAzimuthSteeringRate = 0.0;  // 天线转向速率 (deg/s, TOPS deburst)
    double  masterAzimuthFrequency = 0.0;    // 主影像有效方位向PRF (Hz, 每子条带)
};

// ── 质量报告 ──
struct QualityReport {
    double offsetRmse       = 0.0;   // 偏移残差RMSE (目标 <0.05)
    double meanCorrelation  = 0.0;   // 平均相关系数 (>0.8良好, 0.5-0.8可接受)
    double esdMaxResidual   = 0.0;   // ESD最大残差相位
    int    validPoints      = 0;
    int    totalPoints      = 0;
    QVector<double> perBurstRmse;
    QVector<double> esdPhaseDeltas;
};

#endif // REGISTRATIONTYPES_H
