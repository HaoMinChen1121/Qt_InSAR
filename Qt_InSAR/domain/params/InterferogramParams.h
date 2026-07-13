#ifndef INTERFEROGRAMPARAMS_H
#define INTERFEROGRAMPARAMS_H

#include <QString>

struct InterferogramParams
{
    // 输入 — QSAR产品路径
    QString   masterQsarPath;             // 主影像QSAR路径 (.qsar)
    QString   slaveQsarPath;              // 辅影像QSAR路径 (registered.qsar)
    QString   masterProductDisplay;       // 主产品显示名
    QString   slaveProductDisplay;        // 辅产品显示名

    // 干涉图生成
    int       rangeLooks = 4;            // 距离向多视比 (4→16x缩小)
    int       azimuthLooks = 4;           // 方位向多视比
    QString   outputType = "Complex";     // "Complex" / "Phase" / "Coherence"
    bool      spectralFilter = true;      // 频谱偏移滤波

    // 去平地效应
    QString   referenceSource = "Orbit";  // "Orbit" / "Ellipsoid"
    QString   orbitFilePath;              // 轨道文件路径

    // 差分干涉
    bool      differential = false;
    QString   demPath;                    // DEM文件路径
    QString   displacementDirection = "LOS"; // "LOS" / "Vertical"
    bool      atmosphericCorrection = false;

    // 输出
    QString   outputDir;
    QString   outputPrefix = "interferogram";
    double    incidenceAngle = 35.0;    // 入射角(度), 从主产品获取
    bool      enableFlatEarth = true;
    bool      enableDifferential = false;
};

#endif // INTERFEROGRAMPARAMS_H
