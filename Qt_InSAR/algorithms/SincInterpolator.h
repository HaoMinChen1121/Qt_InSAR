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

// ── 高性能 Sinc 加速 ──

/// 预计算 Sinc×Kaiser 权重表 (256级亚像素精度)
void initSincLUT(int sincWin, double beta,
                 QVector<QVector<float>>& weights);

/// 1D 水平 Sinc — 对 strip 每行做水平插值, dst[row*dstW+col]
void sincInterp1D_Horizontal(const QVector<std::complex<float>>& src,
                              int srcW, int srcH,
                              const QVector<double>& sx,
                              const QVector<QVector<float>>& weightLUT,
                              int sincWin,
                              QVector<std::complex<float>>& dst, int dstW);

/// 1D 垂直 Sinc — 对中间结果每列做垂直插值, dst[width]
void sincInterp1D_Vertical(const QVector<std::complex<float>>& src,
                            int srcH, int width,
                            const QVector<double>& sy,
                            const QVector<QVector<float>>& weightLUT,
                            int sincWin,
                            std::complex<float>* dst);

#endif // REG_SINCINTERPOLATOR_H
