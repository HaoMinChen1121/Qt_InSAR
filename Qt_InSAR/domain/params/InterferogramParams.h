#ifndef INTERFEROGRAMPARAMS_H
#define INTERFEROGRAMPARAMS_H

#include <QString>

struct InterferogramParams
{
    // 干涉图生成
    int       rangeLooks = 1;            // 距离向多视比
    int       azimuthLooks = 1;           // 方位向多视比
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
};

#endif // INTERFEROGRAMPARAMS_H
