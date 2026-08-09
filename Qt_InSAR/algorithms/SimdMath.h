#ifndef SIMDMATH_H
#define SIMDMATH_H

#include <cstdint>
#ifdef _M_X64
#include <intrin.h>   // __cpuidex, _xgetbv
#endif

namespace sar {

// ── SIMD 等级 ──
enum class SimdLevel : int { Scalar = 0, SSE2 = 1, AVX2 = 2 };

inline SimdLevel gSimdLevel = SimdLevel::Scalar;

// 编译期最高支持等级
#ifdef __AVX2__
  constexpr SimdLevel kMaxSimd = SimdLevel::AVX2;
#elif defined(__SSE2__) || defined(_M_X64)
  constexpr SimdLevel kMaxSimd = SimdLevel::SSE2;
#else
  constexpr SimdLevel kMaxSimd = SimdLevel::Scalar;
#endif

// CPUID 检测 (x86/x64)
inline SimdLevel detectSimdLevel() {
#ifdef _M_X64
    int cpuInfo[4];
    __cpuidex(cpuInfo, 1, 0);
    // ECX bit 28 = AVX, bit 27 = OSXSAVE (OS supports AVX state)
    bool hasAvx = (cpuInfo[2] & (1 << 28)) != 0;
    bool hasOsxsave = (cpuInfo[2] & (1 << 27)) != 0;

    if (hasAvx && hasOsxsave) {
        // 检查 OS 是否启用 AVX state (XCR0)
        uint64_t xcr0 = _xgetbv(0);
        bool avxState = (xcr0 & 0x6) == 0x6; // SSE + AVX state

        if (avxState) {
            __cpuidex(cpuInfo, 7, 0);
            // EBX bit 5 = AVX2
            if (cpuInfo[1] & (1 << 5))
                return SimdLevel::AVX2;
            return SimdLevel::SSE2;
        }
    }
    return SimdLevel::SSE2; // x64 必有 SSE2
#else
    return SimdLevel::Scalar;
#endif
}

// ── 辅助: 8-wide 水平求和 ──
#ifdef __AVX2__
#include <immintrin.h>

inline float hsum8(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 sum = _mm_add_ps(lo, hi);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return _mm_cvtss_f32(sum);
}
#endif

// ═══════════════════════════════════════════════════
//  Vertical sinc: 8输出像素 × 1tap 乘加
//  输入: 8个 SoA re/im, 1个广播权重
//  输出: 8对累加器的 re/im 数组 (就地累加)
// ═══════════════════════════════════════════════════

inline void dot8x1Scalar(const float* srcRe, const float* srcIm,
                         float weight, float* accRe, float* accIm) {
    for (int i = 0; i < 8; ++i) {
        accRe[i] += srcRe[i] * weight;
        accIm[i] += srcIm[i] * weight;
    }
}

inline void dot8x1WsumScalar(const float* srcRe, const float* srcIm,
                             float weight, float* accRe, float* accIm,
                             float* wsum) {
    for (int i = 0; i < 8; ++i) {
        accRe[i] += srcRe[i] * weight;
        accIm[i] += srcIm[i] * weight;
        wsum[i] += weight;
    }
}

#ifdef __AVX2__
// AVX2: 8-wide FMA, 一次处理8个输出像素的1个tap
inline void dot8x1AVX2(const float* srcRe, const float* srcIm,
                       __m256 wBroadcast, __m256& accRe, __m256& accIm) {
    __m256 reVec = _mm256_load_ps(srcRe);
    __m256 imVec = _mm256_load_ps(srcIm);
    accRe = _mm256_fmadd_ps(reVec, wBroadcast, accRe);
    accIm = _mm256_fmadd_ps(imVec, wBroadcast, accIm);
}
#endif

// dispatch wrapper
inline void dot8x1(const float* srcRe, const float* srcIm,
                   float weight, float* accRe, float* accIm) {
#ifdef __AVX2__
    if (gSimdLevel >= SimdLevel::AVX2) {
        __m256 w = _mm256_set1_ps(weight);
        __m256 ar = _mm256_load_ps(accRe);
        __m256 ai = _mm256_load_ps(accIm);
        dot8x1AVX2(srcRe, srcIm, w, ar, ai);
        _mm256_store_ps(accRe, ar);
        _mm256_store_ps(accIm, ai);
        return;
    }
#endif
    dot8x1Scalar(srcRe, srcIm, weight, accRe, accIm);
}

// ═══════════════════════════════════════════════════
//  Horizontal sinc: 标量输出像素, 沿tap方向SIMD累加
//  输入: src中连续8个tap的re/im (SoA), 8个连续权重
//  输出: 标量累加器 reSum, imSum (引用累加)
// ═══════════════════════════════════════════════════

inline void tap8Scalar(const float* reData, const float* imData,
                       const float* weight,
                       float& reSum, float& imSum) {
    for (int i = 0; i < 8; ++i) {
        reSum += reData[i] * weight[i];
        imSum += imData[i] * weight[i];
    }
}

#ifdef __AVX2__
inline float tap8AVX2_re(const float* reData, const float* weight8) {
    __m256 r = _mm256_load_ps(reData);
    __m256 w = _mm256_load_ps(weight8);
    return hsum8(_mm256_mul_ps(r, w));
}

inline void tap8AVX2(const float* reData, const float* imData,
                     const float* weight8,
                     float& reSum, float& imSum) {
    reSum += tap8AVX2_re(reData, weight8);
    imSum += tap8AVX2_re(imData, weight8);
}
#endif

inline void tap8(const float* reData, const float* imData,
                 const float* weight8,
                 float& reSum, float& imSum) {
#ifdef __AVX2__
    if (gSimdLevel >= SimdLevel::AVX2) {
        tap8AVX2(reData, imData, weight8, reSum, imSum);
        return;
    }
#endif
    tap8Scalar(reData, imData, weight8, reSum, imSum);
}

// ═══════════════════════════════════════════════════
//  复数旋转 (deramp):  SoA + AVX2
//  out[i] = in[i] * exp(j*dp)  对于连续 n 个元素
// ═══════════════════════════════════════════════════

inline void cplxRotateScalar(const float* reIn, const float* imIn,
                             float* reOut, float* imOut,
                             int n, float dCos, float dSin) {
    for (int i = 0; i < n; ++i) {
        float r = reIn[i], i_ = imIn[i];
        reOut[i] = r * dCos - i_ * dSin;
        imOut[i] = r * dSin + i_ * dCos;
    }
}

#ifdef __AVX2__
inline void cplxRotateAVX2(const float* reIn, const float* imIn,
                           float* reOut, float* imOut,
                           int n, float dCos, float dSin) {
    int vecEnd = n & ~7;
    __m256 cosv = _mm256_set1_ps(dCos);
    __m256 sinv = _mm256_set1_ps(dSin);
    for (int i = 0; i < vecEnd; i += 8) {
        __m256 r = _mm256_load_ps(reIn + i);
        __m256 im = _mm256_load_ps(imIn + i);
        // re' = re*cos - im*sin, im' = re*sin + im*cos
        __m256 reOutV = _mm256_fmsub_ps(r, cosv, _mm256_mul_ps(im, sinv));
        __m256 imOutV = _mm256_fmadd_ps(r, sinv, _mm256_mul_ps(im, cosv));
        _mm256_store_ps(reOut + i, reOutV);
        _mm256_store_ps(imOut + i, imOutV);
    }
    // tail
    for (int i = vecEnd; i < n; ++i) {
        float r = reIn[i], imv = imIn[i];
        reOut[i] = r * dCos - imv * dSin;
        imOut[i] = r * dSin + imv * dCos;
    }
}
#endif

inline void cplxRotate(const float* reIn, const float* imIn,
                       float* reOut, float* imOut,
                       int n, float dCos, float dSin) {
#ifdef __AVX2__
    if (gSimdLevel >= SimdLevel::AVX2) {
        cplxRotateAVX2(reIn, imIn, reOut, imOut, n, dCos, dSin);
        return;
    }
#endif
    cplxRotateScalar(reIn, imIn, reOut, imOut, n, dCos, dSin);
}

} // namespace sar

#endif // SIMDMATH_H
