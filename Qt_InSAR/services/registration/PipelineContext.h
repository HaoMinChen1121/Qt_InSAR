#ifndef PIPELINECONTEXT_H
#define PIPELINECONTEXT_H

#include "domain/params/RegistrationParams.h"
#include "dataaccess/ISarProduct.h"
#include "types/RegistrationTypes.h"

class GdalSlcReader;
class SentinelDataReader;

struct PipelineContext {
    const RegistrationParams* params = nullptr;
    const SarBandDescriptor*  masterBand = nullptr;
    const SarBandDescriptor*  slaveBand  = nullptr;
    int pairIndex  = 0;
    int totalPairs = 0;

    // ── Step 1: 数据 ──
    SlcDataBundle  data;
    QString masterLocalPath;
    QString slaveLocalPath;
    GdalSlcReader* masterReader = nullptr;
    GdalSlcReader* slaveReader  = nullptr;
    SentinelDataReader* masterSdr = nullptr;  // TOPSAR 缓存路径
    SentinelDataReader* slaveSdr  = nullptr;
    bool useBurstCache = false;

    // ── Step 2: Burst ──
    QVector<BurstMatchPair> burstPairs;
    bool isTopsar = false;

    // ── Step 3: 初始偏移 ──
    QVector<BurstOffset> initialOffsets;

    // ── Step 4-5: 观测点 ──
    QVector<OffsetPoint> offsetPoints;

    // ── Step 6: 多项式 ──
    RangePolynomial   rangePoly;
    AzimuthPolynomial aziPoly;

    // ── Step 7: (复用offsetPoints, 精化后更新) ──

    // ── Step 8: ESD ──
    bool esdApplied = false;
    QVector<BurstRegResult> burstResults;

    // ── Step 9: 输出 ──
    QString outputPath;

    // ── Step 10: 质量 ──
    QualityReport qualityReport;

    // ── 控制 ──
    bool    cancelled = false;
    QString errorMessage;
};

#endif // PIPELINECONTEXT_H
