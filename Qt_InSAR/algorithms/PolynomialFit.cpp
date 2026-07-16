#include "PolynomialFit.h"
#include <cmath>
#include <algorithm>
#include <cstdlib>

static bool solveLinear(double* A, double* b, int n) {
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double maxVal = std::abs(A[col * n + col]);
        for (int row = col + 1; row < n; ++row)
            if (std::abs(A[row * n + col]) > maxVal)
                { maxVal = std::abs(A[row * n + col]); pivot = row; }
        if (maxVal < 1e-15) return false;
        if (pivot != col) {
            for (int j = 0; j < n; ++j) std::swap(A[col * n + j], A[pivot * n + j]);
            std::swap(b[col], b[pivot]);
        }
        double piv = A[col * n + col];
        for (int j = col; j < n; ++j) A[col * n + j] /= piv;
        b[col] /= piv;
        for (int row = 0; row < n; ++row) {
            if (row == col) continue;
            double f = A[row * n + col];
            for (int j = col; j < n; ++j) A[row * n + j] -= f * A[col * n + j];
            b[row] -= f * b[col];
        }
    }
    return true;
}

// 单次拟合 (6系数Range + 2系数Azi)
static bool fitOnce(const QVector<OffsetPoint>& pts, int mW, int mH,
                    RangePolynomial& rP, AzimuthPolynomial& aP) {
    int N = pts.size();
    if (N < 6) return false;
    double rATA[36] = {}, rATb[6] = {}, aATA[4] = {}, aATb[2] = {};
    for (const auto& p : pts) {
        double r = (double)p.col / mW, a = (double)p.row / mH;
        double rB[6] = {1, r, a, r*a, r*r, a*a};
        double aB[2] = {1, a};
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 6; ++j) rATA[i*6+j] += rB[i]*rB[j];
            rATb[i] += rB[i] * p.rangeOff;
        }
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) aATA[i*2+j] += aB[i]*aB[j];
            aATb[i] += aB[i] * p.aziOff;
        }
    }
    double rC[36], rB_[6], aC[4], aB_[2];
    std::copy(rATA, rATA+36, rC); std::copy(rATb, rATb+6, rB_);
    std::copy(aATA, aATA+4, aC); std::copy(aATb, aATb+2, aB_);
    if (!solveLinear(rC, rB_, 6) || !solveLinear(aC, aB_, 2)) return false;
    for (int i = 0; i < 6; ++i) rP.coeffs[i] = rB_[i];
    for (int i = 0; i < 2; ++i) aP.coeffs[i] = aB_[i];
    return true;
}

static double residual(const OffsetPoint& p, const RangePolynomial& rP,
                       const AzimuthPolynomial& aP, int mW, int mH) {
    double r = (double)p.col / mW, a = (double)p.row / mH;
    double pR = rP.coeffs[0]+rP.coeffs[1]*r+rP.coeffs[2]*a+rP.coeffs[3]*r*a+rP.coeffs[4]*r*r+rP.coeffs[5]*a*a;
    double pA = aP.coeffs[0]+aP.coeffs[1]*a;
    double dr = p.rangeOff - pR, da = p.aziOff - pA;
    return std::sqrt(dr*dr + da*da);
}

bool fitJointPolynomial(const QVector<OffsetPoint>& points,
                        int masterW, int masterH,
                        int /*burstStartRow*/, int /*burstHeight*/,
                        RangePolynomial& rPoly, AzimuthPolynomial& aPoly)
{
    int N = points.size();
    if (N < 6) return false;

    // RANSAC: 随机采样6点, 拟合, 统计内点, 保留最佳模型
    const int ransacIter = 50;
    const double inlierThresh = 1.0; // 1像素内点阈值
    QVector<bool> bestInliers(N, false);
    int bestCount = 0;
    RangePolynomial bestRP;
    AzimuthPolynomial bestAP;

    for (int iter = 0; iter < ransacIter; ++iter) {
        // 随机选6个点
        QVector<int> idx(N);
        for (int i = 0; i < N; ++i) idx[i] = i;
        for (int i = 0; i < 6; ++i) std::swap(idx[i], idx[i + rand() % (N - i)]);
        QVector<OffsetPoint> sample(6);
        for (int i = 0; i < 6; ++i) sample[i] = points[idx[i]];

        RangePolynomial rP; AzimuthPolynomial aP;
        if (!fitOnce(sample, masterW, masterH, rP, aP)) continue;

        // 计数内点
        QVector<bool> inliers(N, false);
        int cnt = 0;
        for (int i = 0; i < N; ++i) {
            if (residual(points[i], rP, aP, masterW, masterH) < inlierThresh) {
                inliers[i] = true; ++cnt;
            }
        }
        if (cnt > bestCount) {
            bestCount = cnt;
            bestInliers = inliers;
            bestRP = rP; bestAP = aP;
        }
    }

    // 用所有内点做最终最小二乘拟合
    QVector<OffsetPoint> inlierPts;
    for (int i = 0; i < N; ++i)
        if (bestInliers[i]) inlierPts.append(points[i]);

    if (inlierPts.size() < 6) {
        // RANSAC失败, 退化为全点拟合
        inlierPts = points;
    }

    if (!fitOnce(inlierPts, masterW, masterH, rPoly, aPoly))
        return false;

    // RMSE
    double ssR = 0, ssA = 0;
    for (const auto& p : inlierPts) {
        double res = residual(p, rPoly, aPoly, masterW, masterH);
        ssR += res * res;
    }
    rPoly.rmse = std::sqrt(ssR / inlierPts.size());
    aPoly.rmse = rPoly.rmse;

    return true;
}
