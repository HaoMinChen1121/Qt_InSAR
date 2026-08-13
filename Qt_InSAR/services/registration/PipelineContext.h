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
    SarSensorInfo             masterSensorInfo;  // 从主产品XML解析的传感器元数据
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

    // 主影像行 → 辅影像行: 按 burstPairs 匹配结果 + 辅 burst 起始行映射
    // (辅影像 burst 数与主影像不同/时序错位时, 行号不能直接通用)
    int slaveRowFor(int masterRow) const {
        if (!slaveBand) return masterRow;
        int mL = data.linesPerBurst > 0 ? data.linesPerBurst : 1;
        int b = qMax(0, masterRow / mL);
        int local = masterRow - b * mL;
        int sIdx = b;
        if (burstPairs.size() > b && burstPairs[b].isValid
            && burstPairs[b].slaveBurstIdx >= 0) {
            int sc = slaveBand->burstCount;
            if (sc <= 0 || burstPairs[b].slaveBurstIdx < sc)
                sIdx = burstPairs[b].slaveBurstIdx;
        }
        int sL = slaveBand->linesPerBurst > 0 ? slaveBand->linesPerBurst : mL;
        int sRow0 = (slaveBand->burstStartLines.size() > sIdx)
            ? slaveBand->burstStartLines[sIdx] : sIdx * sL;
        if (local >= sL) local = sL - 1;
        return sRow0 + local;
    }

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
