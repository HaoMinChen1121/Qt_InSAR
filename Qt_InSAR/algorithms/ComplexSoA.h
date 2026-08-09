#ifndef COMPLEXSOA_H
#define COMPLEXSOA_H

#include <immintrin.h>   // _mm_malloc, _mm_free
#include <complex>
#include <cstring>

namespace sar {

constexpr int kSimdAlign = 32;

// ── 非拥有视图, 指向外部内存 ──
struct ComplexSoAView {
    float* re = nullptr;
    float* im = nullptr;
    int    size = 0;          // 逻辑元素数
};

// ── 拥有内存的 SoA 缓冲区 ──
struct ComplexSoA {
    float* re = nullptr;
    float* im = nullptr;
    int    size = 0;          // 逻辑元素数
    int    capacity = 0;      // SIMD-padded 分配长度 (8的倍数)

    void alloc(int n) {
        free();
        size = n;
        capacity = (n + 7) & ~7;
        re = static_cast<float*>(_mm_malloc(capacity * sizeof(float), kSimdAlign));
        im = static_cast<float*>(_mm_malloc(capacity * sizeof(float), kSimdAlign));
    }

    void free() {
        if (re) { _mm_free(re); re = nullptr; }
        if (im) { _mm_free(im); im = nullptr; }
        size = capacity = 0;
    }

    ~ComplexSoA() { free(); }

    // AoS -> SoA: 从 std::complex<float> interleaved 数组转换
    void fromAos(const std::complex<float>* src, int n) {
        alloc(n);
        for (int i = 0; i < n; ++i) {
            re[i] = src[i].real();
            im[i] = src[i].imag();
        }
    }

    // AoS -> SoA: 从 CFloat32 interleaved 数组转换
    void fromCfloat32(const void* src, int n) {
        alloc(n);
        const float* f = static_cast<const float*>(src);
        for (int i = 0; i < n; ++i) {
            re[i] = f[i * 2];
            im[i] = f[i * 2 + 1];
        }
    }

    // SoA -> AoS
    void toAos(std::complex<float>* dst, int n) const {
        for (int i = 0; i < n; ++i)
            dst[i] = {re[i], im[i]};
    }

    // 零拷贝子窗口视图
    ComplexSoAView view(int offset, int n) const {
        return {re + offset, im + offset, n};
    }

    ComplexSoA() = default;
    ComplexSoA(ComplexSoA&& o) noexcept
        : re(o.re), im(o.im), size(o.size), capacity(o.capacity)
        { o.re = o.im = nullptr; o.size = o.capacity = 0; }

    ComplexSoA& operator=(ComplexSoA&& o) noexcept {
        if (this != &o) { free(); re = o.re; im = o.im; size = o.size; capacity = o.capacity;
                          o.re = o.im = nullptr; o.size = o.capacity = 0; }
        return *this;
    }

    ComplexSoA(const ComplexSoA&) = delete;
    ComplexSoA& operator=(const ComplexSoA&) = delete;
};

// ── AVX2 加速 deinterleave (8个复数: AoS{re,im} → 两个 __m256) ──
#ifdef __AVX2__

inline void aosToSoa8(const float* __restrict aos,
                      float* __restrict reOut, float* __restrict imOut) {
    // 加载 2×256-bit = 8个{re,im}对
    __m256 ab = _mm256_loadu_ps(aos);         // {r0,i0,r1,i1, r2,i2,r3,i3}
    __m256 cd = _mm256_loadu_ps(aos + 8);     // {r4,i4,r5,i5, r6,i6,r7,i7}

    // 提取实部/虚部
    __m256 re_tmp = _mm256_shuffle_ps(ab, cd, 0x88); // {r0,r1,r4,r5 | r2,r3,r6,r7}
    __m256 im_tmp = _mm256_shuffle_ps(ab, cd, 0xDD); // {i0,i1,i4,i5 | i2,i3,i6,i7}

    // 重排128-bit lane至正确顺序
    __m256i idx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    _mm256_store_ps(reOut, _mm256_permutevar8x32_ps(re_tmp, idx));
    _mm256_store_ps(imOut, _mm256_permutevar8x32_ps(im_tmp, idx));
}

inline void soaToAos8(const float* __restrict reIn, const float* __restrict imIn,
                      float* __restrict aosOut) {
    __m256 reVec = _mm256_load_ps(reIn);
    __m256 imVec = _mm256_load_ps(imIn);

    // interleave: {r0,i0,r1,i1,...} 和 {r4,i4,...} → 两寄存器
    // 分开低/高 128-bit
    __m256 lo = _mm256_unpacklo_ps(reVec, imVec); // {r0,i0,r1,i1, r4,i4,r5,i5}
    __m256 hi = _mm256_unpackhi_ps(reVec, imVec); // {r2,i2,r3,i3, r6,i6,r7,i7}

    // 重排 lane
    __m256i idx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    _mm256_storeu_ps(aosOut,     _mm256_permutevar8x32_ps(lo, idx));
    _mm256_storeu_ps(aosOut + 8, _mm256_permutevar8x32_ps(hi, idx));
}

#endif // __AVX2__

} // namespace sar

#endif // COMPLEXSOA_H
