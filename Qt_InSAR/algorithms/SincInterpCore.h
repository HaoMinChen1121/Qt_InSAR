#ifndef SINCINTERPCORE_H
#define SINCINTERPCORE_H

#include "ComplexSoA.h"
#include "SimdMath.h"
#include <QVector>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

namespace sar {

// ═══════════════════════════════════════════════════
//  sincInterp1D_Horizontal SoA内核
//  策略: 沿tap方向SIMD (每个输出像素的33个taps连续)
// ═══════════════════════════════════════════════════
inline void sincInterp1D_Horizontal_SoA(
    const ComplexSoAView& src, int srcW, int srcH,
    const QVector<double>& sx,
    const QVector<QVector<float>>& weightLUT,
    int sincWin,
    ComplexSoA& dst, int dstW)
{
    dst.alloc(srcH * dstW);
    int K = 2 * sincWin + 1;
    int vecEnd = sincWin - 7; // 最后一组8-tap的起始位置

    for (int row = 0; row < srcH; ++row) {
        int srcRowOff = row * srcW;
        int dstRowOff = row * dstW;
        for (int col = 0; col < dstW; ++col) {
            double x = sx[col];
            int ix = static_cast<int>(std::floor(x));
            int fi = static_cast<int>((x - ix) * 256.0);
            fi = qBound(0, fi, 255);
            const auto& w = weightLUT[fi];

            float sumR = 0, sumI = 0, wsum = 0;

            // 批量处理 8-tap 组
            int i = -sincWin;
            for (; i <= vecEnd; i += 8) {
                int c0 = ix + i, c7 = ix + i + 7;
                if (c0 >= 0 && c7 < srcW) {
                    // 全部在界内: SIMD快速路径
                    int pos = srcRowOff + c0;
                    tap8(src.re + pos, src.im + pos, &w[i + sincWin], sumR, sumI);
                    for (int t = 0; t < 8; ++t)
                        wsum += w[i + t + sincWin];
                } else {
                    for (int t = 0; t < 8; ++t) {
                        int cx = qBound(0, ix + i + t, srcW - 1);
                        float wi = w[i + t + sincWin];
                        int p = srcRowOff + cx;
                        sumR += src.re[p] * wi;
                        sumI += src.im[p] * wi;
                        wsum += wi;
                    }
                }
            }
            // 尾部标量
            for (; i <= sincWin; ++i) {
                int cx = qBound(0, ix + i, srcW - 1);
                float wi = w[i + sincWin];
                int p = srcRowOff + cx;
                sumR += src.re[p] * wi;
                sumI += src.im[p] * wi;
                wsum += wi;
            }

            if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
            dst.re[dstRowOff + col] = sumR;
            dst.im[dstRowOff + col] = sumI;
        }
    }
}

// ═══════════════════════════════════════════════════
//  sincInterp1D_Vertical SoA内核
//  策略: 沿输出像素方向SIMD (8个相邻列 × 同一tap行)
//        要求sy在所有列中相同(TOPSAR fast path)
//        如果不是, 回退到每列标量
// ═══════════════════════════════════════════════════
inline void sincInterp1D_Vertical_SoA(
    const ComplexSoAView& src, int srcH, int width,
    const QVector<double>& sy,
    const QVector<QVector<float>>& weightLUT,
    int sincWin,
    std::complex<float>* dst)
{
    int K = 2 * sincWin + 1;

    // 检查是否所有列共享同一个sy (TOPSAR fast path)
    bool uniformSy = true;
    double sy0 = sy[0];
    for (int c = 1; c < width && uniformSy; ++c)
        uniformSy = (sy[c] == sy0);

    if (uniformSy) {
        int iy = static_cast<int>(std::floor(sy0));
        int fi = static_cast<int>((sy0 - iy) * 256.0);
        fi = qBound(0, fi, 255);
        const auto& w = weightLUT[fi];

        // AVX2: 8输出列并行
#ifdef __AVX2__
        if (gSimdLevel >= SimdLevel::AVX2) {
            int vecCols = width & ~7;
            for (int col = 0; col < vecCols; col += 8) {
                __m256 accRe = _mm256_setzero_ps();
                __m256 accIm = _mm256_setzero_ps();
                __m256 accWsum = _mm256_setzero_ps();

                for (int j = -sincWin; j <= sincWin; ++j) {
                    int ry = qBound(0, iy + j, srcH - 1);
                    int rowOff = ry * width + col;

                    // 检查 tap 行完全在界内
                    if (col + 7 < width) {
                        __m256 reVec = _mm256_load_ps(src.re + rowOff);
                        __m256 imVec = _mm256_load_ps(src.im + rowOff);
                        __m256 wj = _mm256_set1_ps(w[j + sincWin]);
                        accRe = _mm256_fmadd_ps(reVec, wj, accRe);
                        accIm = _mm256_fmadd_ps(imVec, wj, accIm);
                        accWsum = _mm256_add_ps(accWsum, wj);
                    }
                }

                // 存储结果
                float reArr[8], imArr[8], wsArr[8];
                _mm256_store_ps(reArr, accRe);
                _mm256_store_ps(imArr, accIm);
                _mm256_store_ps(wsArr, accWsum);
                for (int c = 0; c < 8; ++c) {
                    if (wsArr[c] > 0) { reArr[c] /= wsArr[c]; imArr[c] /= wsArr[c]; }
                    dst[col + c] = {reArr[c], imArr[c]};
                }
            }
            // 尾部标量
            for (int col = vecCols; col < width; ++col)
                dst[col] = {0, 0};
            for (int col = vecCols; col < width; ++col) {
                float sumR = 0, sumI = 0, wsum = 0;
                for (int j = -sincWin; j <= sincWin; ++j) {
                    int ry = qBound(0, iy + j, srcH - 1);
                    float wj = w[j + sincWin];
                    int p = ry * width + col;
                    sumR += src.re[p] * wj;
                    sumI += src.im[p] * wj;
                    wsum += wj;
                }
                if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
                dst[col] = {sumR, sumI};
            }
            return;
        }
#endif // __AVX2__

        // SSE2/标量: 每列独立
        for (int col = 0; col < width; ++col) {
            float sumR = 0, sumI = 0, wsum = 0;
            for (int j = -sincWin; j <= sincWin; ++j) {
                int ry = qBound(0, iy + j, srcH - 1);
                float wj = w[j + sincWin];
                int p = ry * width + col;
                sumR += src.re[p] * wj;
                sumI += src.im[p] * wj;
                wsum += wj;
            }
            if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
            dst[col] = {sumR, sumI};
        }
        return;
    }

    // sy非均匀: 逐列标量 (保留精确性)
    for (int col = 0; col < width; ++col) {
        double y = sy[col];
        int iy = static_cast<int>(std::floor(y));
        int fi = static_cast<int>((y - iy) * 256.0);
        fi = qBound(0, fi, 255);
        const auto& w = weightLUT[fi];
        float sumR = 0, sumI = 0, wsum = 0;
        for (int j = -sincWin; j <= sincWin; ++j) {
            int ry = qBound(0, iy + j, srcH - 1);
            float wj = w[j + sincWin];
            int p = ry * width + col;
            sumR += src.re[p] * wj;
            sumI += src.im[p] * wj;
            wsum += wj;
        }
        if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
        dst[col] = {sumR, sumI};
    }
}

} // namespace sar

#endif // SINCINTERPCORE_H
