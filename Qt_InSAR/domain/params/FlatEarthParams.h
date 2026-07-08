#ifndef FLATEARTHPARAMS_H
#define FLATEARTHPARAMS_H

#include <QString>

struct FlatEarthParams
{
    QString   method = "Ellipsoid";       // "Ellipsoid" / "Orbit" / "ExternalDem"
    double    semiMajorAxis = 6378137.0;  // 椭球长半轴 (m)
    double    eccentricity = 0.00669438;  // 椭球偏心率
    QString   demPath;                    // 外部DEM路径
    QString   orbitFilePath;
    bool      usePreciseOrbit = true;
};

#endif // FLATEARTHPARAMS_H
