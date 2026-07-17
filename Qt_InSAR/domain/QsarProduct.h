#ifndef QSARPRODUCT_H
#define QSARPRODUCT_H

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>

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

    // TOPSAR burst 元数据 (配准输出携带，用于后续 deburst)
    int     burstCount = 0;
    int     linesPerBurst = 0;
    QVector<int> burstStartLines;
    QVector<QDateTime> burstAzimuthTimes;
    double  azimuthFrequency = 0.0;   // 有效方位向PRF (Hz)
};

// 基线信息 (可选)
struct QsarBaseline {
    double perpendicular = 0;
    double parallel      = 0;
    double temporal      = 0;
    double ambiguityHeight = 0;
};

// 产品描述头 (.qsar JSON 文件)
struct QsarProduct {
    QString  format = "QSAR-1.0";
    QString  productType;
    QString  created;
    QString  sourceMaster;
    QString  sourceSlave;
    QStringList stages;             // ["ifg", "flat", "diff"]
    QsarBaseline baseline;
    QString  coarseMethod;
    QString  resamplingMethod;
    QString  outputPrefix;
    QVector<QsarBand> bands;
};

#endif // QSARPRODUCT_H
