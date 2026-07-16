#ifndef REG_CORRELATION_H
#define REG_CORRELATION_H

#include <complex>
#include <QVector>

// ── 空间域 NCC (幅度域, 滑动窗口搜索) — 粗配准用 ──
double nccCorrelate(const QVector<std::complex<float>>& masterWin,
                    const QVector<std::complex<float>>& slaveWin,
                    int winW, int winH, int searchW, int searchH,
                    int& bestDx, int& bestDy, double& subDx, double& subDy);

// ── FFTW3 幅度域互相关 (幅度 + Hamming窗) — 粗配准用 ──
float fftAmpCorrelate(const std::complex<float>* a,
                      const std::complex<float>* b,
                      float* correlationSurface,
                      int rows, int cols);

// ── FFTW3 相位相关 (复数域 + Hamming窗 + eps归一化) — 精配准用 ──
float fftPhaseCorrelate(const std::complex<float>* a,
                        const std::complex<float>* b,
                        float* correlationSurface,
                        int rows, int cols);

// ── 亚像素峰值定位 ──
void findPeakSubpixel(const float* surface, int outRows, int outCols,
                      double& subDx, double& subDy);

#endif // REG_CORRELATION_H
