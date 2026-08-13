#ifndef REG_POLYNOMIALFIT_H
#define REG_POLYNOMIALFIT_H

#include "services/registration/types/RegistrationTypes.h"
#include <QVector>

/// 联合最小二乘拟合 (所有Burst的OffsetPoint一起参与)
/// Range:  6系数二阶二维  Δr = a0 + a1·r + a2·a + a3·r·a + a4·r² + a5·a²
/// Azimuth: aziOrder 阶模型 (1=[1], 2=[1,a], 3=[1,a,r] 含 range 耦合)
/// burstStartRow/burstHeight: 方位坐标归一化参数
bool fitJointPolynomial(const QVector<OffsetPoint>& points,
                        int masterW, int masterH,
                        int burstStartRow, int burstHeight,
                        int aziOrder,
                        RangePolynomial& rPoly, AzimuthPolynomial& aPoly);

/// 降级拟合: Range [1,r] 一阶 + Azimuth [1,a] 一阶;
/// 各自奇异时退化为常数 (均值)。用于全模型奇异时的兜底。
bool fitReducedPolynomial(const QVector<OffsetPoint>& points,
                          int masterW, int masterH,
                          RangePolynomial& rPoly, AzimuthPolynomial& aPoly);

#endif // REG_POLYNOMIALFIT_H
