#ifndef REG_POLYNOMIALFIT_H
#define REG_POLYNOMIALFIT_H

#include "services/registration/types/RegistrationTypes.h"
#include <QVector>

/// 联合最小二乘拟合 (所有Burst的OffsetPoint一起参与)
/// Range:  6系数二阶二维  Δr = a0 + a1·r + a2·a + a3·r·a + a4·r² + a5·a²
/// Azimuth: 2系数一阶      Δa = b0 + b1·a  (TOPS只拟合方位向一次项)
/// burstStartRow/burstHeight: 方位坐标归一化参数
bool fitJointPolynomial(const QVector<OffsetPoint>& points,
                        int masterW, int masterH,
                        int burstStartRow, int burstHeight,
                        RangePolynomial& rPoly, AzimuthPolynomial& aPoly);

#endif // REG_POLYNOMIALFIT_H
