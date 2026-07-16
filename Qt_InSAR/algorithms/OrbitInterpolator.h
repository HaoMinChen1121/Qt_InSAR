#ifndef ORBITINTERPOLATOR_H
#define ORBITINTERPOLATOR_H

#include "domain/OrbitInfo.h"
#include <QList>

// 三次样条轨道插值
void interpolateOrbit(const QList<OrbitStateVector>& orbits,
                      double t, double& x, double& y, double& z,
                      double& vx, double& vy, double& vz);

// 计算初始轨道偏移 (每burst中心)
void computeOrbitOffset(const QList<OrbitStateVector>& mOrb,
                        const QList<OrbitStateVector>& sOrb,
                        double nearRange, double rangeSpacing,
                        double aziSpacing, double prf,
                        int centerRow, int centerCol,
                        double& rangeOff, double& aziOff);

#endif // ORBITINTERPOLATOR_H
