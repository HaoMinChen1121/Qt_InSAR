#ifndef REG_SINCINTERPOLATOR_H
#define REG_SINCINTERPOLATOR_H

#include <complex>
#include <QVector>

/// Sinc-Kaiser 2D复数插值 (保相)
/// data: [width × height] 复数数据
/// (x, y): 亚像素坐标
/// sincWin: 窗半宽 (8→17点, 16→33点)
/// beta: Kaiser窗参数 (典型2.5)
std::complex<float> sincInterp2D(const QVector<std::complex<float>>& data,
                                  int width, int height,
                                  double x, double y,
                                  int sincWin, double beta);

/// 双线性2D复数插值 (快速回退)
std::complex<float> bilinearInterp2D(const QVector<std::complex<float>>& data,
                                      int width, int height,
                                      double x, double y);

#endif // REG_SINCINTERPOLATOR_H
