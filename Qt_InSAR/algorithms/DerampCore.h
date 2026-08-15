#ifndef DERAMPCORE_H
#define DERAMPCORE_H

#include "ComplexSoA.h"
#include "SimdMath.h"
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace sar {

// ═══════════════════════════════════════════════════
//  Burst 级别一次性 deramp (SoA + SIMD)
//  消除原per-strip的33x冗余计算
//
//  符号约定 (实测验证, 2026-08-15):
//    原始 TOPS 方位相位 = exp(−jπ·kt·η²), kt = annotation azimuthFmRate (负值)
//    → 抵消因子必须为 exp(+jπ·kt·η²), 即 dp = +π·kt·η²
//  此前 dp = −π·kt·η² 与原始 chirp 同号 → 加倍 chirp 而非消除,
//  导致配准输出的辅影像带 2×chirp, 干涉图相干性被摧毁 (coh≈噪声底 0.2)
//
//  距离相关调频率 (2026-08-15 相位质量根因排查, 两轮修正):
//    TOPS 有效调频率 = 轨道项 + 天线转向项: kt_eff(R) = A/R + B,
//    A = −2v²/λ (annotation azimuthFmRate ≈ A/Rmid), B 距离无关。
//    常数 kt deramp 残留 π·Δkt(c)·η² ≈ ±432 rad (burst 边缘) 摧毁相位;
//    而简单 1/R 缩放 kt(c)=kt·Rmid/R(c) 错误地把转向项 B 也缩放,
//    条带边缘残留 B·(Rmid/Rc−1) ≈ ±61 Hz/s → ±454 rad (2026-08-15 实测)。
//    正确模型: kt(c) = kt + ktAnnotation·(Rmid/R(c) − 1)
//    (kt = 实测列平均 = A/Rmid+B; ktAnnotation = 轨道项 ≈ A/Rmid)。
//    ktAnnotation=0 时回退 1/R 缩放; nearRange/rangeSpacing<=0 时常数 kt。
//    16 列分块: 块内 kt 变化 <0.02%, 残余相位 <0.2 rad。
// ═══════════════════════════════════════════════════
inline void applyDeramp_SoA(ComplexSoA& data, int sW, int L,
                             int burstRow0, int burstIdx,
                             double prf, double kt,
                             double nearRange = 0.0, double rangeSpacing = 0.0,
                             double ktAnnotation = 0.0)
{
    constexpr int kColBlock = 16;
    const bool hasGeom = nearRange > 0.0 && rangeSpacing > 0.0;
    const double Rmid = hasGeom ? nearRange + (sW / 2.0) * rangeSpacing : 0.0;
    for (int r = 0; r < L; ++r) {
        int slaveRow = burstRow0 + r;
        int sbIdx = slaveRow / L;
        if (sbIdx < 0) sbIdx = 0;
        if (sbIdx > burstIdx + 1) sbIdx = burstIdx + 1;
        const double eta_S = (slaveRow - sbIdx * L - L / 2.0) / prf;
        const double base = M_PI * eta_S * eta_S;
        int rowOff = r * sW;
        for (int c0 = 0; c0 < sW; c0 += kColBlock) {
            const int n = c0 + kColBlock < sW ? kColBlock : sW - c0;
            double ktEff = kt;
            if (hasGeom) {
                const double Rc = nearRange + (c0 + n * 0.5) * rangeSpacing;
                const double f = Rmid / Rc;
                ktEff = (ktAnnotation != 0.0)
                    ? kt + ktAnnotation * (f - 1.0)
                    : kt * f;
            }
            const double dp = base * ktEff;
            const float dCos = static_cast<float>(std::cos(dp));
            const float dSin = static_cast<float>(std::sin(dp));
            cplxRotate(data.re + rowOff + c0, data.im + rowOff + c0,
                       data.re + rowOff + c0, data.im + rowOff + c0,
                       n, dCos, dSin);
        }
    }
}

// ═══════════════════════════════════════════════════
//  数据驱动 azimuth FM rate 测量
//  2026-08-15 实测: SLC 数据的真实方位 chirp ≈ −1450 Hz/s,
//  而 annotation azimuthFmRate = −2195.78 (比值 ~2/3) — 用 annotation 值
//  deramp 残留 ~+750 Hz/s → 插值核内相位旋转 ~77rad → 重采样输出相位=噪声
//  (幅度幸存、相位摧毁, 不可事后修复)。必须从数据实测 chirp。
//  方法: 逐行圆平均相位差 d[r] ≈ −2π·k·(r−center)/prf²,
//  扫描 k ∈ [−4000, +1000] 使 |Σ d[r]·exp(+j2π·k·(r−center)/prf²)| 最大;
//  返回的 k 即 applyDeramp_SoA 应传入的 kt (数据相位 = −π·k·η² 约定)。
// ═══════════════════════════════════════════════════
inline double measureFmRateFromDiffs(const double* dRe, const double* dIm,
                                     int h, double prf, double centerRow,
                                     double* concentration = nullptr)
{
    if (h < 64 || prf <= 0) { if (concentration) *concentration = 0; return 0.0; }
    // 归一化: 浓度 = |Σ d·exp| / Σ|d| ∈ [0,1] (原始差分为未归一复数积和)
    double sumMag = 0.0;
    for (int r = 0; r < h; ++r)
        sumMag += std::sqrt(dRe[r] * dRe[r] + dIm[r] * dIm[r]);
    if (sumMag < 1e-12) { if (concentration) *concentration = 0; return 0.0; }
    // 数据相位 = −π·kt·η² → 差分 ≈ −2π·kt·(r−c)/prf²;
    // 扫描预测 exp(+j·2π·k·(r−c)/prf²) 匹配于 k = −kt,
    // 故返回 −k 即 applyDeramp_SoA 应传入的 kt。
    // 实测 kt 可正可负且与 annotation 相差 ~750 Hz/s (2026-08-15:
    // 峰值 +1450 → kt=−1450, 而 annotation=−2195.78), 扫描范围须覆盖 ±4000
    auto scoreAt = [&](double k) {
        double sr = 0, si = 0;
        for (int r = 0; r < h; ++r) {
            const double ph = 2.0 * M_PI * k * (r + 0.5 - centerRow) / (prf * prf);
            const double c = std::cos(ph), s = std::sin(ph);
            sr += dRe[r] * c + dIm[r] * s;
            si += dIm[r] * c - dRe[r] * s;
        }
        return std::sqrt(sr * sr + si * si) / sumMag;
    };
    double best = 0.0, bestScore = -1.0;
    for (double k = -4000.0; k <= 4000.0; k += 25.0) {
        const double score = scoreAt(k);
        if (score > bestScore) { bestScore = score; best = k; }
    }
    // ── 精扫 + 抛物线插值 (2026-08-16: 25 Hz/s 粗步长的 ±12.5 Hz/s 误差
    // 造成残余差分 chirp 在 8×8 多视窗内 ~3.6 rad 相位变化 — 摧毁复平均,
    // 窗浓度实测 0.24 的根因; 精化到 ~1 Hz/s 后窗内变化 <0.15 rad) ──
    for (double k = best - 25.0; k <= best + 25.0 + 1e-9; k += 2.0) {
        const double score = scoreAt(k);
        if (score > bestScore) { bestScore = score; best = k; }
    }
    {
        const double s0 = scoreAt(best);
        const double sm = scoreAt(best - 2.0);
        const double sp = scoreAt(best + 2.0);
        const double denom = sm - 2.0 * s0 + sp;
        if (std::abs(denom) > 1e-12) {
            const double off = (sm - sp) / denom;
            if (std::abs(off) <= 2.0) best += off;
        }
    }
    if (concentration) *concentration = bestScore;
    return -best;
}

// AoS 版本 (std::complex<float> 交错布局)
inline double measureAzimuthFmRateAos(const std::complex<float>* data,
                                      int w, int h, double prf, double centerRow,
                                      double* concentration = nullptr)
{
    if (w <= 0 || h < 66) { if (concentration) *concentration = 0; return 0.0; }
    const int colStep = std::max(1, w / 64);   // 采样 ~64 列足够
    const int nDiff = h - 1;                    // 相邻行差分数 (避免读第 h 行越界)
    std::vector<double> dRe(static_cast<size_t>(nDiff)), dIm(static_cast<size_t>(nDiff));
    for (int r = 0; r < nDiff; ++r) {
        const std::complex<float>* r0 = data + static_cast<size_t>(r) * w;
        const std::complex<float>* r1 = r0 + w;
        double sr = 0, si = 0;
        for (int c = 0; c < w; c += colStep) {
            sr += static_cast<double>(r1[c].real()) * r0[c].real()
                + static_cast<double>(r1[c].imag()) * r0[c].imag();
            si += static_cast<double>(r1[c].imag()) * r0[c].real()
                - static_cast<double>(r1[c].real()) * r0[c].imag();
        }
        dRe[r] = sr; dIm[r] = si;
    }
    return measureFmRateFromDiffs(dRe.data(), dIm.data(), nDiff, prf, centerRow, concentration);
}

// SoA 版本 (burst 缓存)
inline double measureAzimuthFmRateSoa(const float* re, const float* im,
                                      int w, int h, double prf, double centerRow,
                                      double* concentration = nullptr)
{
    if (w <= 0 || h < 66) { if (concentration) *concentration = 0; return 0.0; }
    const int colStep = std::max(1, w / 64);
    const int nDiff = h - 1;                    // 相邻行差分数 (避免读第 h 行越界)
    std::vector<double> dRe(static_cast<size_t>(nDiff)), dIm(static_cast<size_t>(nDiff));
    for (int r = 0; r < nDiff; ++r) {
        const float* r0re = re + static_cast<size_t>(r) * w;
        const float* r0im = im + static_cast<size_t>(r) * w;
        const float* r1re = r0re + w;
        const float* r1im = r0im + w;
        double sr = 0, si = 0;
        for (int c = 0; c < w; c += colStep) {
            sr += static_cast<double>(r1re[c]) * r0re[c]
                + static_cast<double>(r1im[c]) * r0im[c];
            si += static_cast<double>(r1im[c]) * r0re[c]
                - static_cast<double>(r1re[c]) * r0im[c];
        }
        dRe[r] = sr; dIm[r] = si;
    }
    return measureFmRateFromDiffs(dRe.data(), dIm.data(), nDiff, prf, centerRow, concentration);
}

} // namespace sar

#endif // DERAMPCORE_H
