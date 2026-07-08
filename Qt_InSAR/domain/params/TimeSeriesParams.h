#ifndef TIMESERIESPARAMS_H
#define TIMESERIESPARAMS_H

#include <QString>
#include <QStringList>

struct TimeSeriesParams {
    QString method;              // "SBAS" / "PS-InSAR" / "Stacking"

    // 像对列表
    QStringList pairFiles;       // 干涉像对描述符文件列表

    // 基线约束
    int    minTemporalBaseline = 30;      // 最小时间基线 (天)
    double maxSpatialBaseline  = 300.0;   // 最大空间基线 (m)
    double maxDopplerDiff      = 0.0;     // 最大多普勒差 (Hz, 0=不约束)

    // 参考点
    QString refPointMethod;      // "Manual" / "Auto" (自动选相干性最高点)
    double  refPointLat = 0.0;
    double  refPointLon = 0.0;
    int     refPointX   = -1;    // 像素坐标 (优先)
    int     refPointY   = -1;

    // 解缠
    QString unwrapMethod;        // "2D" / "3D" / "SNAPHU"
    double  coherenceThreshold = 0.3;

    // 大气校正
    QString atmMethod;           // "GACOS" / "Linear" / "None"
    QString atmDir;              // GACOS 数据目录

    // 非线性形变
    bool    estimateNonlinear = false;
    int     temporalFilterWin = 3;       // 时间滤波窗口

    // 输出
    QString outputDir;
    QString outputPrefix;
    bool    exportRate    = true;
    bool    exportTS       = true;       // 导出每个像元的时序
    bool    exportVelocity = true;
};

#endif // TIMESERIESPARAMS_H
