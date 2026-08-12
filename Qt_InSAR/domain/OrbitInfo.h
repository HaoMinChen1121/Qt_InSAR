#ifndef ORBITINFO_H
#define ORBITINFO_H

#include <QString>
#include <QVector>
#include <QDateTime>

struct OrbitStateVector
{
    QDateTime utcTime;   // 绝对UTC时间 (跨天轨道计算/地球自转补偿)
    double relativeTime; // 相对第一轨秒数 (插值用)
    double x, y, z;      // 位置 (m)
    double vx, vy, vz;   // 速度 (m/s)
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
