#include "FFT.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <QDebug>

#if __has_include(<fftw3.h>)
  #define HAS_FFTW 1
  #include <fftw3.h>
#else
  #define HAS_FFTW 0
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int nextPow2(int n) { int p = 1; while (p < n) p <<= 1; return p; }

static void applyHamming(std::complex<float>* d, int rows, int cols) {
    for (int r = 0; r < rows; ++r) {
        double wy = 0.54 - 0.46 * std::cos(2.0 * M_PI * r / (rows - 1));
        for (int c = 0; c < cols; ++c) {
            double wx = 0.54 - 0.46 * std::cos(2.0 * M_PI * c / (cols - 1));
            d[r * cols + c] *= (float)(wx * wy);
        }
    }
}

static bool fft2D(std::complex<float>* d, int rows, int cols, bool inv) {
#if HAS_FFTW
    static bool once = false;
    if (!once) { qDebug() << "[FFT] FFTW3 ready"; once = true; }
    fftwf_plan p = fftwf_plan_dft_2d(rows, cols,
        reinterpret_cast<fftwf_complex*>(d),
        reinterpret_cast<fftwf_complex*>(d),
        inv ? FFTW_BACKWARD : FFTW_FORWARD, FFTW_ESTIMATE);
    fftwf_execute(p); fftwf_destroy_plan(p);
    if (inv) { float n = 1.0f / (rows * cols); for (int i = 0; i < rows * cols; ++i) d[i] *= n; }
    return true;
#else
    Q_UNUSED(d); Q_UNUSED(rows); Q_UNUSED(cols); Q_UNUSED(inv); return false;
#endif
}

// ── 幅度域互相关 ──
float fftAmpCorrelate(const std::complex<float>* a, const std::complex<float>* b,
                      float* surf, int rows, int cols) {
#if HAS_FFTW
    int N = rows * cols, P = nextPow2(2 * rows - 1), Q = nextPow2(2 * cols - 1), total = P * Q;
    auto FA = std::make_unique<std::complex<float>[]>(total);
    auto FB = std::make_unique<std::complex<float>[]>(total);
    std::memset(FA.get(), 0, total * sizeof(std::complex<float>));
    std::memset(FB.get(), 0, total * sizeof(std::complex<float>));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            FA[r * Q + c] = {std::abs(a[r * cols + c]), 0};
            FB[r * Q + c] = {std::abs(b[r * cols + c]), 0};
        }
    applyHamming(FA.get(), rows, cols);
    applyHamming(FB.get(), rows, cols);
    if (!fft2D(FA.get(), P, Q, false) || !fft2D(FB.get(), P, Q, false)) {
        std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
    }
    for (int i = 0; i < total; ++i) FA[i] = FA[i] * std::conj(FB[i]);
    if (!fft2D(FA.get(), P, Q, true)) {
        std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
    }
    int oR = 2*rows-1, oC = 2*cols-1; float mv = -1e9f;
    for (int r = 0; r < oR; ++r) {
        int sR = (r < rows) ? (P - rows + r) : (r - rows + 1);
        for (int c = 0; c < oC; ++c) {
            int sC = (c < cols) ? (Q - cols + c) : (c - cols + 1);
            float v = FA[sR * Q + sC].real(); surf[r * oC + c] = v;
            if (v > mv) mv = v;
        }
    }
    return mv;
#else
    Q_UNUSED(a); Q_UNUSED(b); std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
#endif
}

// ── 复数域相位相关 ──
float fftPhaseCorrelate(const std::complex<float>* a, const std::complex<float>* b,
                        float* surf, int rows, int cols) {
#if HAS_FFTW
    int P = nextPow2(2 * rows - 1), Q = nextPow2(2 * cols - 1), total = P * Q;
    auto FA = std::make_unique<std::complex<float>[]>(total);
    auto FB = std::make_unique<std::complex<float>[]>(total);
    std::memset(FA.get(), 0, total * sizeof(std::complex<float>));
    std::memset(FB.get(), 0, total * sizeof(std::complex<float>));
    for (int r = 0; r < rows; ++r) {
        std::memcpy(FA.get() + r * Q, a + r * cols, cols * sizeof(std::complex<float>));
        std::memcpy(FB.get() + r * Q, b + r * cols, cols * sizeof(std::complex<float>));
    }
    applyHamming(FA.get(), rows, cols);
    applyHamming(FB.get(), rows, cols);
    if (!fft2D(FA.get(), P, Q, false) || !fft2D(FB.get(), P, Q, false)) {
        std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
    }
    for (int i = 0; i < total; ++i) {
        auto c = FA[i] * std::conj(FB[i]);
        FA[i] = c / (std::abs(c) + 1e-6f);
    }
    if (!fft2D(FA.get(), P, Q, true)) {
        std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
    }
    int oR = 2*rows-1, oC = 2*cols-1; float mv = -1e9f;
    for (int r = 0; r < oR; ++r) {
        int sR = (r < rows) ? (P - rows + r) : (r - rows + 1);
        for (int c = 0; c < oC; ++c) {
            int sC = (c < cols) ? (Q - cols + c) : (c - cols + 1);
            float v = FA[sR * Q + sC].real(); surf[r * oC + c] = v;
            if (v > mv) mv = v;
        }
    }
    return mv;
#else
    Q_UNUSED(a); Q_UNUSED(b); std::fill(surf, surf + (2*rows-1)*(2*cols-1), 0); return 0;
#endif
}

void findPeakSubpixel(const float* surf, int oR, int oC, double& sDx, double& sDy) {
    int pc = oC / 2, pr = oR / 2;
    float mv = surf[pr * oC + pc];
    for (int r = 0; r < oR; ++r)
        for (int c = 0; c < oC; ++c)
            if (surf[r * oC + c] > mv) { mv = surf[r * oC + c]; pr = r; pc = c; }
    double dC = 0, dR = 0;
    if (pc > 0 && pc < oC - 1) {
        float f0 = surf[pr * oC + pc], fm = surf[pr * oC + (pc-1)], fp = surf[pr * oC + (pc+1)];
        dC = (double)(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
    }
    if (pr > 0 && pr < oR - 1) {
        float f0 = surf[pr * oC + pc], fm = surf[(pr-1) * oC + pc], fp = surf[(pr+1) * oC + pc];
        dR = (double)(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
    }
    sDx = (pc + dC) - oC / 2;
    sDy = (pr + dR) - oR / 2;
}
