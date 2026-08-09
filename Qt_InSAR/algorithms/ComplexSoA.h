#ifndef COMPLEXSOA_H
#define COMPLEXSOA_H

#include <immintrin.h>   // _mm_malloc, _mm_free
#include <complex>
#include <cstring>

namespace sar {

constexpr int kSimdAlign = 32;

// ── AVX2 加速 deinterleave/interleave (前置声明, 供 fromAos/toAos 使用) ──
#ifdef __AVX2__
inline void aosToSoa8(const float* __restrict aos,
                      float* __restrict reOut, float* __restrict imOut) {
    __m256 ab = _mm256_loadu_ps(aos);
    __m256 cd = _mm256_loadu_ps(aos + 8);
    __m256 re_tmp = _mm256_shuffle_ps(ab, cd, 0x88);
    __m256 im_tmp = _mm256_shuffle_ps(ab, cd, 0xDD);
    __m256i idx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    _mm256_store_ps(reOut, _mm256_permutevar8x32_ps(re_tmp, idx));
    _mm256_store_ps(imOut, _mm256_permutevar8x32_ps(im_tmp, idx));
}
#endif

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
#ifdef __AVX2__
        int vecEnd = n & ~7;
        const float* sf = reinterpret_cast<const float*>(src);
        for (int i = 0; i < vecEnd; i += 8)
            aosToSoa8(sf + i * 2, re + i, im + i);
        for (int i = vecEnd; i < n; ++i) {
            re[i] = src[i].real();
            im[i] = src[i].imag();
        }
#else
        for (int i = 0; i < n; ++i) {
            re[i] = src[i].real();
            im[i] = src[i].imag();
        }
#endif
    }

    // AoS -> SoA: 从 CFloat32 interleaved 数组转换
    // CFloat32 与 std::complex<float> 布局相同, 复用 AVX2 去交错
    void fromCfloat32(const void* src, int n) {
        alloc(n);
        const float* f = static_cast<const float*>(src);
#ifdef __AVX2__
        int vecEnd = n & ~7;
        for (int i = 0; i < vecEnd; i += 8)
            aosToSoa8(f + i * 2, re + i, im + i);
        for (int i = vecEnd; i < n; ++i) {
            re[i] = f[i * 2];
            im[i] = f[i * 2 + 1];
        }
#else
        for (int i = 0; i < n; ++i) {
            re[i] = f[i * 2];
            im[i] = f[i * 2 + 1];
        }
#endif
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

} // namespace sar

#endif // COMPLEXSOA_H
