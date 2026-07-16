#ifndef PIPELINECONTEXT_H
#define PIPELINECONTEXT_H

#include "domain/params/RegistrationParams.h"
#include "dataaccess/ISarProduct.h"
#include "types/RegistrationTypes.h"

class GdalSlcReader;

struct PipelineContext {
    const RegistrationParams* params = nullptr;
    const SarBandDescriptor*  masterBand = nullptr;
    const SarBandDescriptor*  slaveBand  = nullptr;
    int pairIndex  = 0;
    int totalPairs = 0;

    // ── Step 1: 数据 ──
    SlcDataBundle  data;
    GdalSlcReader* masterReader = nullptr;
    GdalSlcReader* slaveReader  = nullptr;

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
