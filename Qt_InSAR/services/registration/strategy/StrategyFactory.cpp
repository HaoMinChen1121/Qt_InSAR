#include "StrategyFactory.h"

#include <QStringList>

namespace {

// UI 策略解释 (多行)
QString buildSummary(const RegistrationStrategy& s)
{
    QStringList lines;
    lines << QStringLiteral("粗配准: %1").arg(correlationMethodName(s.coarseCorr));
    if (s.useFine)
        lines << QStringLiteral("精配准: %1 亚像素").arg(correlationMethodName(s.fineCorr));
    else
        lines << QStringLiteral("精配准: 未启用");
    switch (s.azimuthModel) {
    case AzimuthOffsetModel::OrbitGeometry:
        lines << QStringLiteral("方位模型: 轨道几何驱动"); break;
    case AzimuthOffsetModel::Correlation:
        lines << QStringLiteral("方位模型: 幅度相关"); break;
    case AzimuthOffsetModel::Hybrid:
        lines << QStringLiteral("方位模型: 轨道几何 + 相关残差"); break;
    case AzimuthOffsetModel::None:
        lines << QStringLiteral("方位模型: 无"); break;
    }
    lines << (s.azimuthCorrection == AzimuthCorrection::ESD
        ? QStringLiteral("ESD: 启用") : QStringLiteral("ESD: 未启用"));
    switch (s.offsetModel) {
    case OffsetModelKind::TOPS:
        lines << QStringLiteral("偏移模型: TOPS"); break;
    case OffsetModelKind::STRIPMAP_2D:
        lines << QStringLiteral("偏移模型: 2D 多项式 (Stripmap)"); break;
    case OffsetModelKind::GENERIC_2D:
        lines << QStringLiteral("偏移模型: 通用 2D"); break;
    }
    lines << QStringLiteral("控制点: %1 / burst").arg(s.defaultOffsetPerBurst);
    lines << QStringLiteral("方位多项式阶数: %1").arg(s.defaultAziOrder);
    return lines.join(QLatin1Char('\n'));
}

// TOPS (IW/EW 共用, 仅模式名不同)
RegistrationStrategy makeTops(ProcessingLevel level)
{
    RegistrationStrategy s;
    s.offsetModel = OffsetModelKind::TOPS;
    s.useBurstMatching = true;

    switch (level) {
    case ProcessingLevel::Fast:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.azimuthModel = AzimuthOffsetModel::OrbitGeometry;
        s.azimuthCorrection = AzimuthCorrection::None;
        s.useFine = false;
        s.defaultOffsetPerBurst = 32;
        s.defaultAziOrder = 2;
        s.note = QStringLiteral(
            "快速模式跳过精配准与 ESD，适合快速浏览。TOPS 方位几何模型不变。");
        break;
    case ProcessingLevel::Standard:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.azimuthModel = AzimuthOffsetModel::OrbitGeometry;
        s.azimuthCorrection = AzimuthCorrection::ESD;
        s.useFine = true;
        s.defaultOffsetPerBurst = 8;
        s.defaultAziOrder = 2;
        s.note = QStringLiteral(
            "标准模式为 S1 TOPS 推荐流程 (接近 SNAP/ISCE)。处理等级不改变 TOPS 方位几何模型。");
        break;
    case ProcessingLevel::High:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.azimuthModel = AzimuthOffsetModel::Hybrid;
        s.azimuthCorrection = AzimuthCorrection::ESD;
        s.useFine = true;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 3;
        s.note = QStringLiteral(
            "高精度模式增加控制点/窗口/模型阶数；方位在轨道几何基础上叠加相关残差 "
            "(Hybrid, 实验性，需要验证)。不会切换为纯相关方位模型。");
        break;
    }

    s.summary = buildSummary(s);
    return s;
}

// Stripmap: 方位用相关; Standard/High 的 FFT_PHASE 为实验性
RegistrationStrategy makeStripmap(ProcessingLevel level)
{
    RegistrationStrategy s;
    s.offsetModel = OffsetModelKind::STRIPMAP_2D;
    s.useBurstMatching = false;
    s.azimuthModel = AzimuthOffsetModel::Correlation;
    s.azimuthCorrection = AzimuthCorrection::None;

    switch (level) {
    case ProcessingLevel::Fast:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = false;
        s.defaultOffsetPerBurst = 32;
        s.defaultAziOrder = 2;
        s.note = QStringLiteral("快速模式: 轨道初值 + FFT 幅度粗配准，无精配准。");
        break;
    case ProcessingLevel::Standard:
        s.coarseCorr = CorrelationMethod::FFT_PHASE;
        s.fineCorr   = CorrelationMethod::FFT_PHASE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 8;
        s.defaultAziOrder = 2;
        s.note = QStringLiteral(
            "实验性，待验证：FFT_PHASE 引擎尚未实现，当前回退到 FFT 幅度相关。");
        break;
    case ProcessingLevel::High:
        s.coarseCorr = CorrelationMethod::FFT_PHASE;
        s.fineCorr   = CorrelationMethod::FFT_PHASE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 3;
        s.note = QStringLiteral(
            "实验性，待验证：FFT_PHASE 引擎尚未实现，当前回退到 FFT 幅度相关；"
            "高阶二维模型需实现后启用。");
        break;
    }

    s.summary = buildSummary(s);
    return s;
}

// ScanSAR: burst 结构 + 轨道方位初值 (实验性)
RegistrationStrategy makeScansar(ProcessingLevel level)
{
    RegistrationStrategy s;
    s.offsetModel = OffsetModelKind::GENERIC_2D;   // SCANSAR 专用模型待实现
    s.useBurstMatching = true;
    s.azimuthModel = AzimuthOffsetModel::OrbitGeometry;
    s.azimuthCorrection = AzimuthCorrection::None;

    switch (level) {
    case ProcessingLevel::Fast:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = false;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 2;
        break;
    case ProcessingLevel::Standard:
        s.coarseCorr = CorrelationMethod::NCC;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 8;
        s.defaultAziOrder = 2;
        break;
    case ProcessingLevel::High:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 3;
        break;
    }

    s.note = QStringLiteral("实验性：ScanSAR 支持需要测试数据验证，偏移模型待专用实现。");
    s.summary = buildSummary(s);
    return s;
}

// Spotlight (实验性)
RegistrationStrategy makeSpotlight(ProcessingLevel level)
{
    RegistrationStrategy s;
    s.offsetModel = OffsetModelKind::GENERIC_2D;   // 聚束二维模型待实现
    s.useBurstMatching = false;
    s.azimuthModel = AzimuthOffsetModel::Correlation;
    s.azimuthCorrection = AzimuthCorrection::None;

    switch (level) {
    case ProcessingLevel::Fast:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = false;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 2;
        break;
    case ProcessingLevel::Standard:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 8;
        s.defaultAziOrder = 2;
        break;
    case ProcessingLevel::High:
        s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
        s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
        s.useFine = true;
        s.defaultOffsetPerBurst = 16;
        s.defaultAziOrder = 3;
        break;
    }

    s.note = QStringLiteral("实验性：聚束模式支持需要测试数据验证，二维模型待专用实现。");
    s.summary = buildSummary(s);
    return s;
}

// 未知产品: 通用回退 (语义上 ≠ STRIPMAP, UI 必须警示)
RegistrationStrategy makeGeneric(ProcessingLevel level)
{
    RegistrationStrategy s;
    s.offsetModel = OffsetModelKind::GENERIC_2D;
    s.useBurstMatching = false;
    s.azimuthModel = AzimuthOffsetModel::Correlation;
    s.azimuthCorrection = AzimuthCorrection::None;
    s.coarseCorr = CorrelationMethod::FFT_AMPLITUDE;
    s.fineCorr   = CorrelationMethod::FFT_AMPLITUDE;
    s.useFine = (level != ProcessingLevel::Fast);
    s.defaultOffsetPerBurst = 8;
    s.defaultAziOrder = 2;
    s.note = QStringLiteral(
        "未知产品模式，已使用通用回退策略，结果需要人工验证。");
    s.summary = buildSummary(s);
    return s;
}

} // namespace

RegistrationStrategy StrategyFactory::create(ProductMode mode, ProcessingLevel level)
{
    switch (mode) {
    case ProductMode::TOPS_IW:
    case ProductMode::TOPS_EW:
        return makeTops(level);
    case ProductMode::STRIPMAP:
        return makeStripmap(level);
    case ProductMode::SCANSAR:
        return makeScansar(level);
    case ProductMode::SPOTLIGHT:
        return makeSpotlight(level);
    case ProductMode::GENERIC:
    default:
        return makeGeneric(level);
    }
}
