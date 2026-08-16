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

// ── 方位向多项式: Δa = b0 + b1·a + b2·r (range-dependent azimuth) ──
struct AzimuthPolynomial {
    double coeffs[3] = {};
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
    int samplesPerBurst = 0;
    QVector<int>       burstStartLines;
    QVector<QDateTime> masterBurstTimes;
    QVector<QDateTime> slaveBurstTimes;
    double  slaveAzimuthFmRate = 0.0;       // 辅影像方位向调频率 (Hz/s, TOPS deramp)
    double  slaveAzimuthSteeringRate = 0.0;  // 天线转向速率 (deg/s, TOPS deburst)
    double  masterAzimuthFrequency = 0.0;    // 主影像有效方位向PRF (Hz, 每子条带)
};

// ── 质量报告 ──
struct QualityReport {
    double offsetRmse       = 0.0;   // 偏移残差RMSE (2D 合并, 目标 <0.05)
    double rangeRmse        = 0.0;   // 距离向残差RMSE (px, 单独诊断)
    double aziRmse          = 0.0;   // 方位向残差RMSE (px, 单独诊断)
    double polyRangeRmse    = 0.0;   // 距离多项式拟合RMSE (Step6, ESD前)
    double polyAziRmse      = 0.0;   // 方位多项式拟合RMSE (Step6, ESD前)
    double meanCorrelation  = 0.0;   // 平均相关系数 (>0.8良好, 0.5-0.8可接受)
    double esdMaxResidual   = 0.0;   // ESD最大残差相位
    int    validPoints      = 0;
    int    totalPoints      = 0;
    // ── 残差 mean/std (区分系统偏差与随机噪声; mean 大=配准错位) ──
    double rangeOffsetMean  = 0.0;
    double rangeOffsetStd   = 0.0;
    double aziOffsetMean    = 0.0;
    double aziOffsetStd     = 0.0;
    double aziResidualRangeSlope = 0.0;  // 方位残差-vs-归一化距离 线性斜率 (px/rn)
    double aziPolyRangeCoeff     = 0.0;  // 方位模型 r 项 (ESD β, px/rn)
    bool   esdDone         = false;
    bool   resamplingDone  = false;
    // ── 配准后互相关验证 (主 vs 注册辅, 振幅域; 峰值位置=残余失配) ──
    double postCoregResidualRange   = 0.0;   // 残余距离偏移 (px, 亚像素, 均值)
    double postCoregResidualAzimuth = 0.0;   // 残余方位偏移 (px, 亚像素, 均值)
    double postCoregPeakValue       = 0.0;   // 归一化相关峰值 (场景/时间基线相关, 仅记录)
    int    postCoregWindows         = 0;
    QVector<double> postCoregWindowRangeRes;
    QVector<double> postCoregWindowAziRes;
    QVector<double> postCoregWindowPeak;
    QVector<double> perBurstRmse;       // 2D 合并
    QVector<double> perBurstRangeRmse;  // 距离向 (诊断 burst 级结构)
    QVector<double> perBurstAziRmse;    // 方位向
    QVector<double> esdPhaseDeltas;
};

#endif // REGISTRATIONTYPES_H
