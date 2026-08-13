#include "ProductDetector.h"

#include <QDebug>

ProductMode ProductDetector::detect(const SarSensorInfo& si, int burstCount,
                                    QString* warning)
{
    const QString mode = si.acquisitionMode.toUpper();
    const QString sensor = si.sensorType.toUpper();

    ProductMode result = ProductMode::GENERIC;

    if (sensor.contains(QStringLiteral("SENTINEL-1"))) {
        if (mode == QStringLiteral("IW"))
            result = ProductMode::TOPS_IW;
        else if (mode == QStringLiteral("EW"))
            result = ProductMode::TOPS_EW;
        else if (mode == QStringLiteral("SM"))
            result = ProductMode::STRIPMAP;
    } else if (mode == QStringLiteral("SM")) {
        result = ProductMode::STRIPMAP;   // 通用 SAR 规则
    }

    // burstCount 仅作一致性校验, 不改变识别结果
    if ((result == ProductMode::TOPS_IW || result == ProductMode::TOPS_EW)
        && burstCount <= 1) {
        qWarning() << "[ProductDetector] TOPS 模式但 burstCount<=1, 结果需要人工验证";
        if (warning)
            *warning = QStringLiteral("TOPS 模式但未检测到 burst 结构，结果需要人工验证。");
    }

    if (si.productType != SarProductType::SLC) {
        qWarning() << "[ProductDetector] 非 SLC 产品, 配准结果需要人工验证";
        if (warning)
            *warning = QStringLiteral("非 SLC 产品，配准结果需要人工验证。");
    }

    if (result == ProductMode::GENERIC) {
        qWarning() << "[ProductDetector] 未知产品模式 (sensor=" << si.sensorType
                   << "mode=" << si.acquisitionMode << ") 使用通用回退策略";
    }

    return result;
}
