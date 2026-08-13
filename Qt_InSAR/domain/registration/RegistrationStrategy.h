#ifndef REG_REGISTRATIONSTRATEGY_H
#define REG_REGISTRATIONSTRATEGY_H

#include <QString>
#include "domain/registration/ProductMode.h"
#include "domain/registration/ProcessingLevel.h"
#include "domain/registration/CorrelationMethod.h"
#include "domain/params/RegistrationParams.h"

// ═══════════════════════════════════════════════════════════
//  配准策略 (纯数据): 算法 + 物理模型 + 开关 + UI 解释
//  由 StrategyFactory 按 ProductMode × ProcessingLevel 生成,
//  再经 applyOverrides 应用用户覆盖 (仅粗配准引擎可覆盖)
// ═══════════════════════════════════════════════════════════

// Offset 的解释模型 — 决定相关观测如何被拟合为参数模型
enum class OffsetModelKind {
    TOPS,        // Δa = f(r, a, ΔfDC, Ka, burst) + 逐 burst 修正
    STRIPMAP_2D, // Δr = f(r,a), Δa = g(r,a)  (近似)
    GENERIC_2D   // Stripmap-like 通用回退
};

enum class AzimuthOffsetModel {
    None,
    OrbitGeometry, // 轨道几何插值 (TOPS 方位硬结论, 不可被等级改变)
    Correlation,   // 方位幅度相关 (Stripmap)
    Hybrid         // 轨道初值 + 相关残差 (预留/实验性)
};

enum class AzimuthCorrection {
    None,
    ESD
    // SpectralDiversity 预留
};

struct RegistrationStrategy {
    CorrelationMethod  coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
    CorrelationMethod  fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
    OffsetModelKind    offsetModel = OffsetModelKind::GENERIC_2D;
    AzimuthOffsetModel azimuthModel = AzimuthOffsetModel::Correlation;
    AzimuthCorrection  azimuthCorrection = AzimuthCorrection::None;
    bool useFine = false;
    bool useBurstMatching = false;
    int  defaultOffsetPerBurst = 8;
    int  defaultAziOrder = 2;    // 1=[1] 2=[1,a] 3=[1,a,r]
    QString summary;   // UI 策略解释: 粗/精配准、方位模型、ESD、偏移模型、控制点
    QString note;      // 补充说明 (实验性标注 / 通用回退警示 / 等级不改变物理模型)

    // 应用用户覆盖 (唯一开放项: 粗配准引擎; ESD/方位模型仍由策略决定)
    void applyOverrides(const RegistrationParams& p) {
        if (p.coarseCorrOverride.has_value())
            coarseCorr = p.coarseCorrOverride.value();
    }
};

#endif // REG_REGISTRATIONSTRATEGY_H
