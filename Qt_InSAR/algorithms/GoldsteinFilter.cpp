#include "GoldsteinFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if __has_include(<fftw3.h>)
  #define HAS_FFTW 1
  #include <fftw3.h>
#else
  #define HAS_FFTW 0
#endif

namespace sar {

namespace {

// 无效像素阈值: 幅度平方小于该值视为无效 (零填充/掩膜区)
constexpr float kInvalidMag2 = 1e-6f;

// 一维三角窗权重 (距 patch 中心越远越小, 50% 重叠时任意像素权重和恒定)
inline double blendWeight1D(int x, int patch)
{
    const double half = patch * 0.5;
    return 1.0 - std::abs(x - (patch - 1) * 0.5) / half;
}

// 谱幅度 3×3 平滑 (wrap-around, 覆盖 DC 与 Nyquist 邻域)
template<int W>
void smoothMagnitude(float* mag, int n)
{
    std::vector<float> buf(static_cast<size_t>(n) * n);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            float s = 0;
            for (int dr = -W / 2; dr <= W / 2; ++dr)
                for (int dc = -W / 2; dc <= W / 2; ++dc) {
                    const int rr = (r + dr + n) % n;
                    const int cc = (c + dc + n) % n;
                    s += mag[static_cast<size_t>(rr) * n + cc];
                }
            buf[static_cast<size_t>(r) * n + c] = s / static_cast<float>(W * W);
        }
    }
    std::memcpy(mag, buf.data(), sizeof(float) * static_cast<size_t>(n) * n);
}

struct GoldFilterCtx
{
#if HAS_FFTW
    fftwf_plan fwd = nullptr;
    fftwf_plan inv = nullptr;
    // FFTW 边界用 reinterpret_cast (std::complex<float> 布局与 fftwf_complex 兼容,
    // 与 Correlation.cpp 惯例一致); vector<fftwf_complex> 不可行 (float[2] 数组
    // 元素无法用圆括号初始化, C3074)
    std::vector<std::complex<float>> fftBuf;
#endif
    std::vector<float> mag;
};

bool ensurePlans(GoldFilterCtx& ctx, int patch)
{
#if HAS_FFTW
    if (ctx.fwd && ctx.inv) return true;
    const int n2 = patch * patch;
    ctx.fftBuf.resize(n2);
    ctx.fwd = fftwf_plan_dft_2d(patch, patch,
        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
        FFTW_FORWARD, FFTW_ESTIMATE);
    ctx.inv = fftwf_plan_dft_2d(patch, patch,
        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
        FFTW_BACKWARD, FFTW_ESTIMATE);
    return ctx.fwd && ctx.inv;
#else
    Q_UNUSED(ctx); Q_UNUSED(patch);
    return false;
#endif
}

} // namespace

std::vector<std::complex<float>> goldsteinFilter(
    const std::complex<float>* ifg, int w, int h,
    double alpha, int patch, int smoothWin, int tile)
{
    std::vector<std::complex<float>> out;
    out.resize(static_cast<size_t>(w) * h);
    if (!ifg || w <= 0 || h <= 0) return out;
    std::memcpy(out.data(), ifg, sizeof(std::complex<float>) * static_cast<size_t>(w) * h);

#if !HAS_FFTW
    (void)alpha; (void)patch; (void)smoothWin; (void)tile;
    return out;
#endif
#if HAS_FFTW
    alpha = std::clamp(alpha, 0.0, 1.0);
    if (smoothWin < 3 || (smoothWin % 2) == 0) smoothWin = 3;
    // patch 收敛到不超过图像的最小 2 的幂
    int p = 1;
    const int maxP = std::min(w, h);
    while (p * 2 <= patch && p * 2 <= maxP) p *= 2;
    if (p < 4 || maxP < 4) return out;
    patch = p;
    if (tile < patch) tile = patch;

    GoldFilterCtx ctx;
    if (!ensurePlans(ctx, patch)) return out;
    std::vector<float>& mag = ctx.mag;
    mag.resize(static_cast<size_t>(patch) * patch);

    // 无效像素掩膜 (幅度≈0)
    std::vector<unsigned char> invalid(static_cast<size_t>(w) * h, 0);
    {
        for (long i = 0; i < static_cast<long>(w) * h; ++i) {
            const auto& v = ifg[i];
            if (v.real() * v.real() + v.imag() * v.imag() < kInvalidMag2)
                invalid[i] = 1;
        }
    }

    // 累积缓冲: 实部/虚部/权重 (每像素最多被 4 个 patch 覆盖, float 精度足够)
    std::vector<float> accRe(static_cast<size_t>(w) * h, 0.0f);
    std::vector<float> accIm(static_cast<size_t>(w) * h, 0.0f);
    std::vector<float> accW(static_cast<size_t>(w) * h, 0.0f);

    std::vector<std::complex<float>> patchBuf(static_cast<size_t>(patch) * patch);

    const int step = patch / 2;   // 50% 重叠
    for (int tileR0 = 0; tileR0 < h; tileR0 += tile) {
        const int tileR1 = std::min(tileR0 + tile, h);
        for (int tileC0 = 0; tileC0 < w; tileC0 += tile) {
            const int tileC1 = std::min(tileC0 + tile, w);
            for (int r0 = tileR0; r0 < tileR1; r0 += step) {
                for (int c0 = tileC0; c0 < tileC1; c0 += step) {
                    // 镜像填充读取 patch
                    bool anyValid = false;
                    for (int r = 0; r < patch; ++r) {
                        int gr = r0 + r;
                        if (gr >= h) gr = 2 * h - 2 - gr;   // 镜像
                        if (gr < 0) gr = -gr;
                        for (int c = 0; c < patch; ++c) {
                            int gc = c0 + c;
                            if (gc >= w) gc = 2 * w - 2 - gc;
                            if (gc < 0) gc = -gc;
                            gr = std::clamp(gr, 0, h - 1);
                            gc = std::clamp(gc, 0, w - 1);
                            const long gi = static_cast<long>(gr) * w + gc;
                            std::complex<float> v = invalid[gi] ? std::complex<float>(0, 0)
                                                                : ifg[gi];
                            if (v != std::complex<float>(0, 0)) anyValid = true;
                            patchBuf[static_cast<size_t>(r) * patch + c] = v;
                        }
                    }
                    if (!anyValid) continue;

                    // FFT → 谱幅度平滑^α 加权 → IFFT
                    std::memcpy(ctx.fftBuf.data(), patchBuf.data(),
                                sizeof(std::complex<float>) * static_cast<size_t>(patch) * patch);
                    fftwf_execute_dft(ctx.fwd,
                        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
                        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()));
                    for (int i = 0; i < patch * patch; ++i)
                        mag[i] = std::abs(ctx.fftBuf[i]);
                    if (smoothWin == 3) smoothMagnitude<3>(mag.data(), patch);
                    else if (smoothWin == 5) smoothMagnitude<5>(mag.data(), patch);
                    else smoothMagnitude<3>(mag.data(), patch);

                    for (int i = 0; i < patch * patch; ++i) {
                        const float m = mag[i];
                        const float g = m > 1e-12f
                            ? std::pow(m, static_cast<float>(alpha)) / m : 0.0f;
                        ctx.fftBuf[i] *= g;
                    }
                    fftwf_execute_dft(ctx.inv,
                        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()),
                        reinterpret_cast<fftwf_complex*>(ctx.fftBuf.data()));
                    const float norm = 1.0f / static_cast<float>(patch * patch);

                    // 三角加权累积
                    for (int r = 0; r < patch; ++r) {
                        const int gr = r0 + r;
                        if (gr < 0 || gr >= h) continue;
                        const float wv = static_cast<float>(blendWeight1D(r, patch));
                        for (int c = 0; c < patch; ++c) {
                            const int gc = c0 + c;
                            if (gc < 0 || gc >= w) continue;
                            const long gi = static_cast<long>(gr) * w + gc;
                            if (invalid[gi]) continue;
                            const float wt = wv * static_cast<float>(blendWeight1D(c, patch));
                            const auto& v = ctx.fftBuf[static_cast<size_t>(r) * patch + c];
                            accRe[gi] += v.real() * norm * wt;
                            accIm[gi] += v.imag() * norm * wt;
                            accW[gi] += wt;
                        }
                    }
                }
            }
        }
    }

    for (long i = 0; i < static_cast<long>(w) * h; ++i) {
        if (invalid[i]) continue;   // 无效像素保留原值
        if (accW[i] > 1e-12f)
            out[i] = std::complex<float>(accRe[i] / accW[i], accIm[i] / accW[i]);
    }
    return out;
#endif
}

} // namespace sar
