#ifndef ORBITINFO_H
#define ORBITINFO_H

#include <QString>
#include <QVector>

struct OrbitStateVector
{
    double time;       // 时间 (s)
    double x, y, z;    // 位置 (m)
    double vx, vy, vz; // 速度 (m/s)
};

struct OrbitInfo
{
    QString                      sensorName;
    QString                      orbitDirection;  // "Ascending" / "Descending"
    QVector<OrbitStateVector>    stateVectors;    // 轨道状态向量序列
    double                       prf = 0;
    double                       rangeSamplingRate = 0;
};

#endif // ORBITINFO_H
