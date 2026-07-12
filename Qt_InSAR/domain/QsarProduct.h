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
    QString file;           // 相对TIFF文件名
    int     width  = 0;
    int     height = 0;
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
    QString  productType;           // "RegisteredSLC", "Interferogram", "Unwrapped", ...
    QString  created;               // ISO datetime
    QString  sourceMaster;          // 主产品来源
    QString  sourceSlave;           // 辅产品来源
    QsarBaseline baseline;
    QString  coarseMethod;
    QString  resamplingMethod;
    QString  outputPrefix;
    QVector<QsarBand> bands;
};

#endif // QSARPRODUCT_H
