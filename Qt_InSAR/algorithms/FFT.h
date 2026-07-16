#ifndef ALGORITHMS_FFT_H
#define ALGORITHMS_FFT_H

#include <complex>

// ── FFTW3 幅度域互相关 — 粗配准用 (256×256推荐) ──
// 提取幅度, Hamming窗, FFTW3, 互相关 (不归一化, 抗SAR噪声)
// 返回最大相关值, surface=[(2*rows-1)×(2*cols-1)]
float fftAmpCorrelate(const std::complex<float>* a,
                      const std::complex<float>* b,
                      float* correlationSurface,
                      int rows, int cols);

// ── FFTW3 相位相关 — 精配准用 (256×256推荐) ──
// 复数域, Hamming窗, 互功率谱归一化 cross/(|cross|+eps)
float fftPhaseCorrelate(const std::complex<float>* a,
                        const std::complex<float>* b,
                        float* correlationSurface,
                        int rows, int cols);

// ── 亚像素峰值 — 抛物线拟合 ──
void findPeakSubpixel(const float* surface, int outRows, int outCols,
                      double& subDx, double& subDy);

#endif
