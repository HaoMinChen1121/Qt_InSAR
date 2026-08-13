#ifndef REG_PRODUCTMODE_H
#define REG_PRODUCTMODE_H

#include <QString>

// 产品物理模式 — 决定配准物理模型 (由 ProductDetector 自动识别)
enum class ProductMode {
    TOPS_IW,     // Sentinel-1 IW (TOPS, 9 burst/子条带)
    TOPS_EW,     // Sentinel-1 EW
    STRIPMAP,    // 条带模式 SLC
    SCANSAR,     // 扫描模式 (burst 结构, 实验性)
    SPOTLIGHT,   // 聚束模式 (实验性)
    GENERIC      // 未知产品: 通用回退 (语义上 ≠ STRIPMAP)
};

inline QString productModeName(ProductMode m) {
    switch (m) {
    case ProductMode::TOPS_IW:   return QStringLiteral("S1 IW TOPS");
    case ProductMode::TOPS_EW:   return QStringLiteral("S1 EW TOPS");
    case ProductMode::STRIPMAP:  return QStringLiteral("Stripmap");
    case ProductMode::SCANSAR:   return QStringLiteral("ScanSAR");
    case ProductMode::SPOTLIGHT: return QStringLiteral("Spotlight");
    case ProductMode::GENERIC:   return QStringLiteral("未知产品模式");
    }
    return QString();
}

#endif // REG_PRODUCTMODE_H
