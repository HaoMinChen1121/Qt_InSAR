#ifndef RANGESPECTRALFILTER_H
#define RANGESPECTRALFILTER_H

#include <complex>
#include <vector>
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if __has_include(<fftw3.h>)
  #define RSF_HAS_FFTW 1
  #include <fftw3.h>
#else
  #define RSF_HAS_FFTW 0
#endif

// ═══════════════════════════════════════════════════════
//  距离向公共频带滤波 (common-band filtering)
//  2026-08-16 相位质量根因排查: B⊥=3310m 的像对距离向频谱平移
//  Δf = f0·B⊥/(R·tanθ) ≈ 29 MHz vs 带宽 56.5 MHz → 频谱重叠仅 49%
//  → 基线去相关 γ_geom ≈ 0.49 (平移不变, 2D 亚像素搜索实测相干面
//  全平 0.23 = 0.49×时间去相关 — 与理论吻合)。
//  主辅两侧各保留公共频带 |f| < BW/2 − |Δf|/2 (对称截断, 与符号无关),
//  外缘余弦渐变窗; 逐距离行 FFT → 掩膜 → IFFT。
// ═══════════════════════════════════════════════════════

namespace sar {

class RangeSpectralFilter
{
public:
    RangeSpectralFilter() = default;
    ~RangeSpectralFilter() { release(); }

    // n: 距离向样本数; bw: 距离向带宽 (Hz); deltaF: 频谱平移 (Hz, 取绝对值)
    bool init(int n, double bw, double deltaF)
    {
#if RSF_HAS_FFTW
        release();
        if (n < 8 || bw <= 0 || deltaF <= 0 || deltaF >= bw * 0.95) return false;
        mN = n;
        mBuf.resize(n);
        mFwd = fftwf_plan_dft_1d(n,
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            FFTW_FORWARD, FFTW_ESTIMATE);
        mInv = fftwf_plan_dft_1d(n,
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            FFTW_BACKWARD, FFTW_ESTIMATE);
        if (!mFwd || !mInv) { release(); return false; }
        // 频域掩膜: f(k) = (k<=N/2 ? k : k-N)·bw/N
        // W = 1 当 |f| ≤ fKeep; cos² 渐变至 fStop; 0 超出
        const double fKeep = bw / 2.0 - deltaF / 2.0;   // 保留半宽
        const double fStop = bw / 2.0;                  // 截止半宽
        mMask.resize(n);
        for (int k = 0; k < n; ++k) {
            const double f = (k <= n / 2 ? k : k - n) * bw / n;
            const double af = std::abs(f);
            double w = 1.0;
            if (af > fKeep) {
                if (af >= fStop) w = 0.0;
                else {
                    const double t = (af - fKeep) / (fStop - fKeep);
                    const double c = std::cos(M_PI / 2.0 * t);
                    w = c * c;
                }
            }
            mMask[k] = static_cast<float>(w);
        }
        return true;
#else
        (void)n; (void)bw; (void)deltaF;
        return false;
#endif
    }

    // 就地逐行滤波 (row 长度 = init 的 n)
    void apply(std::complex<float>* row)
    {
#if RSF_HAS_FFTW
        if (!mFwd) return;
        std::memcpy(mBuf.data(), row, sizeof(std::complex<float>) * mN);
        fftwf_execute_dft(mFwd,
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            reinterpret_cast<fftwf_complex*>(mBuf.data()));
        for (int k = 0; k < mN; ++k)
            mBuf[k] *= mMask[k];
        fftwf_execute_dft(mInv,
            reinterpret_cast<fftwf_complex*>(mBuf.data()),
            reinterpret_cast<fftwf_complex*>(mBuf.data()));
        const float norm = 1.0f / static_cast<float>(mN);
        for (int k = 0; k < mN; ++k)
            row[k] = mBuf[k] * norm;
#else
        (void)row;
#endif
    }

    bool active() const { return mFwd != nullptr; }

private:
    void release()
    {
#if RSF_HAS_FFTW
        if (mFwd) { fftwf_destroy_plan(mFwd); mFwd = nullptr; }
        if (mInv) { fftwf_destroy_plan(mInv); mInv = nullptr; }
#endif
    }
#if RSF_HAS_FFTW
    fftwf_plan mFwd = nullptr;
    fftwf_plan mInv = nullptr;
#endif
    int mN = 0;
    std::vector<std::complex<float>> mBuf;
    std::vector<float> mMask;
};

} // namespace sar

#endif // RANGESPECTRALFILTER_H
