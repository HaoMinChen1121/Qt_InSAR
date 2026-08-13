#ifndef REG_CORRELATION_H
#define REG_CORRELATION_H

#include <complex>
#include <QVector>
#include "domain/registration/CorrelationMethod.h"

// ── 空间域 NCC (幅度域, 滑动窗口搜索) — 粗配准用 ──
double nccCorrelate(const QVector<std::complex<float>>& masterWin,
                    const QVector<std::complex<float>>& slaveWin,
                    int winW, int winH, int searchW, int searchH,
                    int& bestDx, int& bestDy, double& subDx, double& subDy);

// ── FFTW3 幅度域互相关 (幅度 + Hamming窗) — 粗/精配准用 ──
float fftAmpCorrelate(const std::complex<float>* a,
                      const std::complex<float>* b,
                      float* correlationSurface,
                      int rows, int cols);

// ── FFTW3 相位相关 (复数域 + Hamming窗 + 归一化互功率谱) ──
// 峰值 ∈ [-1,1] 归一化相关系数; 实验性 (Stripmap 矩阵项)
float fftPhaseCorrelate(const std::complex<float>* a,
                        const std::complex<float>* b,
                        float* correlationSurface,
                        int rows, int cols);

// ── 引擎分派: 按策略方法选择相关引擎 (NCC 由调用方走滑动搜索) ──
float correlateSurface(const std::complex<float>* a,
                       const std::complex<float>* b,
                       float* correlationSurface,
                       int rows, int cols,
                       CorrelationMethod method);

// ── 亚像素峰值定位 ──
void findPeakSubpixel(const float* surface, int outRows, int outCols,
                      double& subDx, double& subDy);

#endif // REG_CORRELATION_H
