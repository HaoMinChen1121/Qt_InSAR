#include "SincInterpolator.h"
#include <cmath>
#include <algorithm>
#include <QtGlobal>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

double sinc(double x) {
    if (std::abs(x) < 1e-8) return 1.0;
    return std::sin(M_PI * x) / (M_PI * x);
}

double besselI0(double x) {
    double ax = std::abs(x);
    if (ax < 3.75) {
        double y = (x / 3.75) * (x / 3.75);
        return 1.0 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492
            + y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
    }
    double y = 3.75 / ax;
    return (std::exp(ax) / std::sqrt(ax)) * (0.39894228
        + y * (0.01328592 + y * (0.00225319 + y * (-0.00157565
        + y * (0.00916281 + y * (-0.02057706 + y * (0.02635537
        + y * (-0.01647633 + y * 0.00392377))))))));
}

double kaiserWindow(int n, int N, double beta) {
    double arg = beta * std::sqrt(1.0 - std::pow(2.0 * n / (N - 1) - 1.0, 2.0));
    return besselI0(arg) / besselI0(beta);
}

} // anonymous

std::complex<float> sincInterp2D(const QVector<std::complex<float>>& data,
                                  int width, int height,
                                  double x, double y,
                                  int sincWin, double beta)
{
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    double fx = x - ix, fy = y - iy;

    double valR = 0, valI = 0, wsum = 0;
    int N = 2 * sincWin + 1;

    for (int c = -sincWin; c <= sincWin; ++c) {
        int cx = qBound(0, ix + c, width - 1);
        double wx = sinc(c - fx) * kaiserWindow(c + sincWin, N, beta);
        double colR = 0, colI = 0, cwsum = 0;
        for (int r = -sincWin; r <= sincWin; ++r) {
            int ry = qBound(0, iy + r, height - 1);
            double wy = sinc(r - fy) * kaiserWindow(r + sincWin, N, beta);
            double w = wx * wy;
            auto v = data[ry * width + cx];
            colR += v.real() * w;
            colI += v.imag() * w;
            cwsum += w;
        }
        if (cwsum > 0) { colR /= cwsum; colI /= cwsum; }
        valR += colR; valI += colI; wsum += 1;
    }
    if (wsum > 0) { valR /= wsum; valI /= wsum; }
    return {static_cast<float>(valR), static_cast<float>(valI)};
}

std::complex<float> bilinearInterp2D(const QVector<std::complex<float>>& data,
                                      int width, int height,
                                      double x, double y)
{
    int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = qBound(0, x0, width - 1);  x1 = qBound(0, x1, width - 1);
    y0 = qBound(0, y0, height - 1); y1 = qBound(0, y1, height - 1);
    double fx = x - std::floor(x), fy = y - std::floor(y);
    auto f00 = data[y0 * width + x0], f10 = data[y0 * width + x1];
    auto f01 = data[y1 * width + x0], f11 = data[y1 * width + x1];
    float re = (float)((1-fx)*(1-fy)*f00.real() + fx*(1-fy)*f10.real()
        + (1-fx)*fy*f01.real() + fx*fy*f11.real());
    float im = (float)((1-fx)*(1-fy)*f00.imag() + fx*(1-fy)*f10.imag()
        + (1-fx)*fy*f01.imag() + fx*fy*f11.imag());
    return {re, im};
}

// ── 预计算 Sinc×Kaiser 权重表 ──
void initSincLUT(int sincWin, double beta,
                 QVector<QVector<float>>& weights)
{
    int K = 2 * sincWin + 1;
    weights.resize(256);
    for (int frac = 0; frac < 256; ++frac) {
        double fx = frac / 256.0;
        weights[frac].resize(K);
        for (int i = -sincWin; i <= sincWin; ++i) {
            weights[frac][i + sincWin] = (float)(
                sinc(i - fx) * kaiserWindow(i + sincWin, K, beta));
        }
    }
}

// ── 1D 水平 Sinc 插值 ──
void sincInterp1D_Horizontal(const QVector<std::complex<float>>& src,
                              int srcW, int srcH,
                              const QVector<double>& sx,
                              const QVector<QVector<float>>& weightLUT,
                              int sincWin,
                              QVector<std::complex<float>>& dst, int dstW)
{
    dst.resize(srcH * dstW);
    for (int row = 0; row < srcH; ++row) {
        int srcRowOff = row * srcW;
        int dstRowOff = row * dstW;
        for (int col = 0; col < dstW; ++col) {
            double x = sx[col];
            int ix = (int)std::floor(x);
            int fi = (int)((x - ix) * 256.0);
            fi = qBound(0, fi, 255);
            const auto& w = weightLUT[fi];
            float sumR = 0, sumI = 0, wsum = 0;
            for (int i = -sincWin; i <= sincWin; ++i) {
                int cx = qBound(0, ix + i, srcW - 1);
                auto v = src[srcRowOff + cx];
                float wi = w[i + sincWin];
                sumR += v.real() * wi;
                sumI += v.imag() * wi;
                wsum += wi;
            }
            if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
            dst[dstRowOff + col] = {sumR, sumI};
        }
    }
}

// ── 1D 垂直 Sinc 插值 ──
void sincInterp1D_Vertical(const QVector<std::complex<float>>& src,
                            int srcH, int width,
                            const QVector<double>& sy,
                            const QVector<QVector<float>>& weightLUT,
                            int sincWin,
                            std::complex<float>* dst)
{
    for (int col = 0; col < width; ++col) {
        double y = sy[col];
        int iy = (int)std::floor(y);
        int fi = (int)((y - iy) * 256.0);
        fi = qBound(0, fi, 255);
        const auto& w = weightLUT[fi];
        float sumR = 0, sumI = 0, wsum = 0;
        for (int j = -sincWin; j <= sincWin; ++j) {
            int ry = qBound(0, iy + j, srcH - 1);
            auto v = src[ry * width + col];
            float wj = w[j + sincWin];
            sumR += v.real() * wj;
            sumI += v.imag() * wj;
            wsum += wj;
        }
        if (wsum > 0) { sumR /= wsum; sumI /= wsum; }
        dst[col] = {sumR, sumI};
    }
}
