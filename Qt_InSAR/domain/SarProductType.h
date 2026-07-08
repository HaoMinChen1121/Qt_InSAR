#ifndef SARPRODUCTTYPE_H
#define SARPRODUCTTYPE_H

#include <QString>

enum class SarProductType {
    SLC,    // Single Look Complex — 单视复数 (干涉可用)
    GRD,    // Ground Range Detected — 地距检测 (幅度图)
    RTC,    // Radiometric Terrain Corrected — 辐射地形校正
    RAW,    // Raw signal data — 原始信号 (Level-0)
    Unknown
};

inline QString sarProductTypeToString(SarProductType t) {
    switch (t) {
    case SarProductType::SLC: return QStringLiteral("SLC");
    case SarProductType::GRD: return QStringLiteral("GRD");
    case SarProductType::RTC: return QStringLiteral("RTC");
    case SarProductType::RAW: return QStringLiteral("RAW");
    default: return QStringLiteral("Unknown");
    }
}

inline SarProductType sarProductTypeFromString(const QString& s) {
    if (s.contains("SLC", Qt::CaseInsensitive)) return SarProductType::SLC;
    if (s.contains("GRD", Qt::CaseInsensitive)) return SarProductType::GRD;
    if (s.contains("RTC", Qt::CaseInsensitive)) return SarProductType::RTC;
    if (s.contains("RAW", Qt::CaseInsensitive)) return SarProductType::RAW;
    return SarProductType::Unknown;
}

#endif // SARPRODUCTTYPE_H
