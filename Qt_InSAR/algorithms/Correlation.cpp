#include "Correlation.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>

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
static QMutex gFftwMutex;  // FFTW3 plan创建不是线程安全的

// ── Hamming 窗 ──
static void applyHammingWindow(std::complex<float>* data, int rows, int cols) {
    for (int r = 0; r < rows; ++r) {
        double wy = 0.54 - 0.46 * std::cos(2.0 * M_PI * r / (rows - 1));
        for (int c = 0; c < cols; ++c) {
            double wx = 0.54 - 0.46 * std::cos(2.0 * M_PI * c / (cols - 1));
            data[r * cols + c] *= (float)(wx * wy);
        }
    }
}

// ── FFTW3 2D FFT ──
static bool fft2D(std::complex<float>* data, int rows, int cols, bool inverse) {
#if HAS_FFTW
    static bool logged = false;
    if (!logged) { qDebug() << "[Correlation] FFTW3 ready"; logged = true; }
    fftwf_plan p;
    {
        QMutexLocker lock(&gFftwMutex);
        p = fftwf_plan_dft_2d(rows, cols,
            reinterpret_cast<fftwf_complex*>(data),
            reinterpret_cast<fftwf_complex*>(data),
            inverse ? FFTW_BACKWARD : FFTW_FORWARD, FFTW_ESTIMATE);
    }
    fftwf_execute(p);
    {
        QMutexLocker lock(&gFftwMutex);
        fftwf_destroy_plan(p);
    }
    if (inverse) {
        float norm = 1.0f / (rows * cols);
        for (int i = 0; i < rows * cols; ++i) data[i] *= norm;
    }
    return true;
#else
    Q_UNUSED(data); Q_UNUSED(rows); Q_UNUSED(cols); Q_UNUSED(inverse); return false;
#endif
}

// ── FFTW3 相位相关 (复数域 + Hamming窗 + 归一化互功率谱) ──
float fftPhaseCorrelate(const std::complex<float>* a,
                        const std::complex<float>* b,
                        float* correlationSurface, int rows, int cols)
{
#if HAS_FFTW
    int P = nextPow2(2 * rows - 1), Q = nextPow2(2 * cols - 1), total = P * Q;

    auto FA = std::make_unique<std::complex<float>[]>(total);
    auto FB = std::make_unique<std::complex<float>[]>(total);
    std::memset(FA.get(), 0, total * sizeof(std::complex<float>));
    std::memset(FB.get(), 0, total * sizeof(std::complex<float>));

    // 复制 + Hamming窗
    for (int r = 0; r < rows; ++r) {
        std::memcpy(FA.get() + r * Q, a + r * cols, cols * sizeof(std::complex<float>));
        std::memcpy(FB.get() + r * Q, b + r * cols, cols * sizeof(std::complex<float>));
    }
    applyHammingWindow(FA.get(), rows, cols);
    applyHammingWindow(FB.get(), rows, cols);

    // FFT
    if (!fft2D(FA.get(), P, Q, false) || !fft2D(FB.get(), P, Q, false)) {
        std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0); return 0;
    }

    // 相位相关: R = F_A * conj(F_B) / (|F_A * conj(F_B)| + eps)
    for (int i = 0; i < total; ++i) {
        auto cross = FA[i] * std::conj(FB[i]);
        FA[i] = cross / (std::abs(cross) + 1e-6f);
    }

    if (!fft2D(FA.get(), P, Q, true)) {
        std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0); return 0;
    }

    int outRows = 2 * rows - 1, outCols = 2 * cols - 1;
    float maxV = -1e9f;
    for (int r = 0; r < outRows; ++r) {
        int srcR = (r < rows) ? (P - rows + r) : (r - rows + 1);
        for (int c = 0; c < outCols; ++c) {
            int srcC = (c < cols) ? (Q - cols + c) : (c - cols + 1);
            float v = FA[srcR * Q + srcC].real();
            correlationSurface[r * outCols + c] = v;
            if (v > maxV) maxV = v;
        }
    }
    return maxV;
#else
    Q_UNUSED(a); Q_UNUSED(b);
    std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0);
    return 0;
#endif
}

// ── FFTW3 幅度域互相关 (幅度提取 + Hamming窗 + 互相关, 不归一化) ──
float fftAmpCorrelate(const std::complex<float>* a,
                      const std::complex<float>* b,
                      float* correlationSurface, int rows, int cols)
{
#if HAS_FFTW
    int N = rows * cols;
    std::vector<std::complex<float>> ampA(N), ampB(N);
    for (int i = 0; i < N; ++i) {
        ampA[i] = {std::abs(a[i]), 0};
        ampB[i] = {std::abs(b[i]), 0};
    }

    int P = nextPow2(2 * rows - 1), Q = nextPow2(2 * cols - 1), total = P * Q;
    auto FA = std::make_unique<std::complex<float>[]>(total);
    auto FB = std::make_unique<std::complex<float>[]>(total);
    std::memset(FA.get(), 0, total * sizeof(std::complex<float>));
    std::memset(FB.get(), 0, total * sizeof(std::complex<float>));

    for (int r = 0; r < rows; ++r) {
        std::memcpy(FA.get() + r * Q, ampA.data() + r * cols, cols * sizeof(std::complex<float>));
        std::memcpy(FB.get() + r * Q, ampB.data() + r * cols, cols * sizeof(std::complex<float>));
    }
    applyHammingWindow(FA.get(), rows, cols);
    applyHammingWindow(FB.get(), rows, cols);

    if (!fft2D(FA.get(), P, Q, false) || !fft2D(FB.get(), P, Q, false)) {
        std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0); return 0;
    }

    for (int i = 0; i < total; ++i)
        FA[i] = FA[i] * std::conj(FB[i]);

    if (!fft2D(FA.get(), P, Q, true)) {
        std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0); return 0;
    }

    int outRows = 2 * rows - 1, outCols = 2 * cols - 1;
    float maxV = -1e9f;
    for (int r = 0; r < outRows; ++r) {
        int srcR = (r < rows) ? (P - rows + r) : (r - rows + 1);
        for (int c = 0; c < outCols; ++c) {
            int srcC = (c < cols) ? (Q - cols + c) : (c - cols + 1);
            float v = FA[srcR * Q + srcC].real();
            correlationSurface[r * outCols + c] = v;
            if (v > maxV) maxV = v;
        }
    }
    return maxV;
#else
    Q_UNUSED(a); Q_UNUSED(b);
    std::fill(correlationSurface, correlationSurface + (2*rows-1)*(2*cols-1), 0);
    return 0;
#endif
}

// ── 亚像素峰值定位 ──
void findPeakSubpixel(const float* surface, int outRows, int outCols,
                      double& subDx, double& subDy)
{
    int pc = outCols / 2, pr = outRows / 2;
    float maxV = surface[pr * outCols + pc];
    for (int r = 0; r < outRows; ++r)
        for (int c = 0; c < outCols; ++c)
            if (surface[r * outCols + c] > maxV)
                { maxV = surface[r * outCols + c]; pr = r; pc = c; }

    double dC = 0, dR = 0;
    if (pc > 0 && pc < outCols - 1) {
        float f0 = surface[pr * outCols + pc];
        float fm = surface[pr * outCols + (pc-1)];
        float fp = surface[pr * outCols + (pc+1)];
        dC = (double)(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
    }
    if (pr > 0 && pr < outRows - 1) {
        float f0 = surface[pr * outCols + pc];
        float fm = surface[(pr-1) * outCols + pc];
        float fp = surface[(pr+1) * outCols + pc];
        dR = (double)(fp - fm) / (2.0 * (2.0 * f0 - fm - fp) + 1e-10);
    }
    subDx = (pc + dC) - outCols / 2;
    subDy = (pr + dR) - outRows / 2;
}

// ── 空间域 NCC (幅度域, 粗配准用) ──
double nccCorrelate(const QVector<std::complex<float>>& masterWin,
                    const QVector<std::complex<float>>& slaveWin,
                    int winW, int winH, int searchW, int searchH,
                    int& bestDx, int& bestDy, double& subDx, double& subDy)
{
    QVector<double> mAmp(winW * winH); double mMagSum = 0;
    for (int i = 0; i < winW * winH; ++i) {
        mAmp[i] = std::abs(masterWin[i]);
        mMagSum += mAmp[i] * mAmp[i];
    }
    double mMagNorm = std::sqrt(std::max(1e-15, mMagSum));
    int sr = (searchH - winH)/2, sc = (searchW - winW)/2;
    if (sr < 0) sr = 0; if (sc < 0) sc = 0;

    QVector<double> corrMap((sr*2+1)*(sc*2+1));
    bestDx = bestDy = 0; double bestNcc = -1;
    for (int dy = -sr; dy <= sr; ++dy) {
        for (int dx = -sc; dx <= sc; ++dx) {
            int ox = sc+dx, oy = sr+dy;
            double cross = 0, sMagSum = 0;
            for (int y = 0; y < winH; ++y)
                for (int x = 0; x < winW; ++x) {
                    double sv = std::abs(slaveWin[(oy+y)*searchW + (ox+x)]);
                    cross += mAmp[y*winW+x] * sv; sMagSum += sv*sv;
                }
            double ncc = cross / (mMagNorm * std::sqrt(std::max(1e-15, sMagSum)));
            corrMap[(dy+sr)*(sc*2+1)+(dx+sc)] = ncc;
            if (ncc > bestNcc) { bestNcc = ncc; bestDx = dx; bestDy = dy; }
        }
    }
    int cs = sc*2+1, rs = sr*2+1, pc = bestDx+sc, pr = bestDy+sr;
    if (pc>0 && pc<cs-1 && pr>0 && pr<rs-1) {
        auto v = [&](int c, int r) { return corrMap[r*cs + c]; };
        double f00=v(pc,pr), fm1=v(pc-1,pr), fp1=v(pc+1,pr), f0m1=v(pc,pr-1), f0p1=v(pc,pr+1);
        double dC=2*(2*f00-fm1-fp1), dR=2*(2*f00-f0m1-f0p1);
        subDx = pc + (dC!=0 ? (fp1-fm1)/dC : 0) - sc;
        subDy = pr + (dR!=0 ? (f0p1-f0m1)/dR : 0) - sr;
    } else { subDx = bestDx; subDy = bestDy; }
    return bestNcc;
}
