#ifndef QSARPRODUCT_H
#define QSARPRODUCT_H

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>
#include <QVariantMap>
#include "domain/OrbitInfo.h"

// 单个波段描述
struct QsarBand {
    QString subSwath;
    QString polarization;
    QString file;               // 默认指向 ifg (兼容旧版)
    QString ifgFile;            // 干涉图 (ifg/IW1_VH_ifg.tif)
    QString cohFile;            // 相干性
    QString phaseFile;          // 原始相位
    QString flatFile;           // 去平地复数
    QString flatPhaseFile;      // 去平地相位
    QString diffFile;           // 差分复数
    QString diffPhaseFile;      // 差分相位
    int     width  = 0;
    int     height = 0;

    // 图层显示控制
    QString layerType = "phase";       // "complex"/"phase"/"coherence"/"flat_phase"/"diff_phase"
    bool    defaultVisible = false;    // 干涉图完成后是否自动加载到画布

    // TOPSAR burst 元数据 (配准输出携带，用于后续 deburst)
    int     burstCount = 0;
    int     linesPerBurst = 0;
    QVector<int> burstStartLines;
    QVector<QDateTime> burstAzimuthTimes;
    double  azimuthFrequency = 0.0;   // 有效方位向PRF (Hz)
};

// 基线信息 (legacy v1 顶层字段, 保留兼容读取; 新写走 ProductMetadata.baseline)
struct QsarBaseline {
    double perpendicular = 0;
    double parallel      = 0;
    double temporal      = 0;
    double ambiguityHeight = 0;
};

// ═══════════════════════════════════════════════════════════
//  Product Metadata (schema v2.0, 产品自描述)
//  qsar 只存摘要; 详细报告 (质量/几何表) 放外部 json 由摘要引用
// ═══════════════════════════════════════════════════════════

// 轨道 (产品级, master/slave 各一份)
struct QsarOrbitMeta {
    QString source;                 // "annotation" / "precise"
    QString direction;              // "Ascending" / "Descending"
    QString referenceTime;          // ISO 首状态向量 UTC 时间
    QVector<OrbitStateVector> stateVectors;
};

// 像对信息 (pair 语义: baseline 属于 pair 而非任一产品)
struct QsarPairInfo {
    QString masterId;
    QString slaveId;
    QString masterTime;             // ISO 采集开始时间
    QString slaveTime;
};

// 基线 (pair 级, 配准阶段由两轨道计算)
struct QsarBaselineMeta {
    bool   valid = false;
    double perpendicular = 0.0;
    double parallel      = 0.0;
    double temporal      = 0.0;     // 天
    double ambiguityHeight = 0.0;
};

// 配准处理参数快照 (可追溯性)
struct QsarRegistrationMeta {
    QString coarseMethod;
    int     coarseWindowSize   = 0;
    int     coarseSearchWindow = 0;
    int     fineWindowSize     = 0;
    int     polynomialDegree   = 0;
    double  correlationThreshold = 0.0;
    QString resamplingMethod;
    int     sincWindowSize     = 0;
};

// 干涉处理参数快照
struct QsarInterferogramMeta {
    int    rangeLooks   = 0;
    int    azimuthLooks = 0;
    double wavelength   = 0.0;     // 雷达波长 (m), 形变转换直接取用
    double inputRangeSpacing   = 0.0;
    double inputAzimuthSpacing = 0.0;
    double outputRangeSpacing  = 0.0;
    double outputAzimuthSpacing = 0.0;
};

struct QsarProcessingMeta {
    bool hasRegistration  = false;
    QsarRegistrationMeta registration;
    bool hasInterferogram = false;
    QsarInterferogramMeta interferogram;
};

// 几何摘要 (完整逐列表在 file 指向的 geom json)
struct QsarGeometryMeta {
    QString model;                  // "geom_table"
    QString file;                   // 相对路径
    double  nearRange = 0.0;
    double  farRange  = 0.0;
    double  incMin    = 0.0;        // rad
    double  incMax    = 0.0;
};

// 质量摘要 (详细报告在 detailFile)
struct QsarQualityMeta {
    double meanCorrelation = 0.0;   // 配准: 平均相关系数
    double offsetRmse      = 0.0;   // 配准: 偏移残差 RMSE
    double validRatio      = 0.0;
    double meanCoherence   = 0.0;   // 干涉: 平均相干性
    bool   unwrapReady     = false;
    QString detailFile;             // 相对路径
};

// TOPS 元数据 (canonical 位置, 每子条带一份; band 内 burst 字段为 legacy 兼容)
struct QsarTopsBurst {
    int     index        = 0;
    int     startLine    = 0;
    QString azimuthTime;            // ISO
    double  esdCorrection = 0.0;    // ESD 方位修正 (像元), 配准阶段写入
    // 多普勒质心多项式 (dataDcPoly, 配准阶段从辅影像 annotation 落盘):
    // f_DC(τ) = Σ dcPoly[k]·(τ − dcT0)^k, τ 为双程斜距时间 (s)
    QVector<double> dcPoly;
    double  dcT0 = 0.0;
};
struct QsarTopsSwath {
    QString name;
    int     burstCount = 0;
    int     linesPerBurst = 0;
    double  azimuthFrequency = 0.0;
    QVector<QsarTopsBurst> bursts;
};
struct QsarTopsMeta {
    QVector<QsarTopsSwath> swaths;
};

// 处理历史 (结构化 stages)
struct QsarStageRecord {
    QString name;
    QString time;                   // ISO
    QString softwareVersion;
    QVariantMap params;
};

// 产品元数据汇总
struct ProductMetadata {
    QsarPairInfo       pair;
    QsarOrbitMeta      orbitMaster;
    QsarOrbitMeta      orbitSlave;
    QsarBaselineMeta   baseline;
    QsarProcessingMeta processing;
    QsarGeometryMeta   geometry;
    QsarQualityMeta    quality;
    QsarTopsMeta       tops;
};

// 产品描述头 (.qsar JSON 文件)
struct QsarProduct {
    QString  format = "QSAR-1.0";
    QString  schemaVersion = "2.0";
    QString  productType;
    QString  created;
    QString  sourceMaster;
    QString  sourceSlave;
    QStringList stages;             // legacy v1 名称数组 ["ifg", "flat", "diff"]
    QVector<QsarStageRecord> history;   // v2 结构化处理历史
    QsarBaseline baseline;          // legacy v1 顶层基线 (读取时与 metadata 同步)
    QString  coarseMethod;
    QString  resamplingMethod;
    QString  outputPrefix;
    QVector<QsarBand> bands;
    ProductMetadata metadata;
};

#endif // QSARPRODUCT_H
