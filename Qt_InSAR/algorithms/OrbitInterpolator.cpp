#include "OrbitInterpolator.h"
#include <QVector>
#include <cmath>

namespace {

struct CubicSpline {
    QVector<double> t, y, a, b, c, d;
    int n = 0;

    CubicSpline(const QVector<double>& times, const QVector<double>& values) {
        n = times.size();
        if (n < 2) return;
        t = times; y = values;
        a.resize(n); b.resize(n); c.resize(n); d.resize(n);
        QVector<double> h(n - 1), alpha(n - 1);
        for (int i = 0; i < n - 1; ++i) {
            h[i] = t[i + 1] - t[i];
            if (h[i] > 0) {
                a[i] = y[i];
                // 自然样条: i==0 只有右差分项 (左端二阶导=0);
                // 旧代码对 i==0 也读 y[i-1] (越界读 → 堆垃圾 → 样条污染)
                alpha[i] = (3.0 / h[i]) * (y[i + 1] - y[i]);
                if (i > 0)
                    alpha[i] -= (3.0 / h[i - 1]) * (y[i] - y[i - 1]);
            }
        }
        QVector<double> l(n), mu(n), z(n);
        l[0] = 1.0; mu[0] = 0.0; z[0] = 0.0;
        for (int i = 1; i < n - 1; ++i) {
            l[i] = 2.0 * (t[i + 1] - t[i - 1]) - h[i - 1] * mu[i - 1];
            if (std::abs(l[i]) < 1e-12) l[i] = 1e-12;
            mu[i] = h[i] / l[i];
            z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
        }
        l[n - 1] = 1.0; z[n - 1] = 0.0; c[n - 1] = 0.0;
        for (int j = n - 2; j >= 0; --j) {
            c[j] = z[j] - mu[j] * c[j + 1];
            b[j] = (y[j + 1] - y[j]) / h[j] - h[j] * (c[j + 1] + 2.0 * c[j]) / 3.0;
            d[j] = (c[j + 1] - c[j]) / (3.0 * h[j]);
        }
    }

    double eval(double x) const {
        if (n < 2) return y.isEmpty() ? 0.0 : y[0];
        if (x <= t[0]) return y[0];
        if (x >= t[n - 1]) return y[n - 1];
        int i = 0;
        for (int j = 0; j < n - 1; ++j)
            if (x >= t[j] && x <= t[j + 1]) { i = j; break; }
        double dx = x - t[i];
        return a[i] + b[i] * dx + c[i] * dx * dx + d[i] * dx * dx * dx;
    }
};

} // anonymous

void interpolateOrbit(const QList<OrbitStateVector>& orbits,
                      double t, double& x, double& y, double& z,
                      double& vx, double& vy, double& vz)
{
    int n = orbits.size();
    if (n < 2) { x = y = z = vx = vy = vz = 0; return; }
    QVector<double> times(n), xs(n), ys(n), zs(n), vxs(n), vys(n), vzs(n);
    for (int i = 0; i < n; ++i) {
        times[i] = orbits[i].relativeTime;
        xs[i] = orbits[i].x; ys[i] = orbits[i].y; zs[i] = orbits[i].z;
        vxs[i] = orbits[i].vx; vys[i] = orbits[i].vy; vzs[i] = orbits[i].vz;
    }
    CubicSpline sx(times, xs), sy(times, ys), sz(times, zs);
    CubicSpline svx(times, vxs), svy(times, vys), svz(times, vzs);
    x = sx.eval(t); y = sy.eval(t); z = sz.eval(t);
    vx = svx.eval(t); vy = svy.eval(t); vz = svz.eval(t);
}

void computeOrbitOffset(const QList<OrbitStateVector>& mOrb,
                        const QList<OrbitStateVector>& sOrb,
                        double nearRange, double rangeSpacing,
                        double aziSpacing, double prf,
                        int centerRow, int centerCol,
                        double& rangeOff, double& aziOff)
{
    if (mOrb.size() < 2 || sOrb.size() < 2) {
        rangeOff = 0; aziOff = 0; return;
    }
    double t0 = (mOrb.first().relativeTime + mOrb.last().relativeTime) * 0.5;
    double tAzi = t0 + centerRow / prf;

    // 线性插值 (10s 间隔轨道矢量, 与 BaselineEstimator 一致;
    // 三次样条在逐 burst 采样点上产生 ±210m 级数值振荡)
    auto lerpOrbit = [](const QList<OrbitStateVector>& orb, double t,
                        double& x, double& y, double& z,
                        double& vx, double& vy, double& vz) {
        int i0 = 0;
        for (int i = 0; i < orb.size() - 1; ++i) {
            if (orb[i].relativeTime <= t && orb[i + 1].relativeTime >= t) {
                i0 = i; break;
            }
        }
        const auto& s0 = orb[i0];
        const auto& s1 = orb[qMin(i0 + 1, orb.size() - 1)];
        double dt = s1.relativeTime - s0.relativeTime;
        double f = (std::abs(dt) > 1e-9) ? (t - s0.relativeTime) / dt : 0.0;
        f = qMax(0.0, qMin(1.0, f));
        x = s0.x + f * (s1.x - s0.x);
        y = s0.y + f * (s1.y - s0.y);
        z = s0.z + f * (s1.z - s0.z);
        vx = s0.vx + f * (s1.vx - s0.vx);
        vy = s0.vy + f * (s1.vy - s0.vy);
        vz = s0.vz + f * (s1.vz - s0.vz);
    };

    double mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t;
    double sx_t, sy_t, sz_t, svx_t, svy_t, svz_t;
    lerpOrbit(mOrb, tAzi, mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t);
    lerpOrbit(sOrb, tAzi, sx_t, sy_t, sz_t, svx_t, svy_t, svz_t);

    double mVmag = std::sqrt(mvx_t * mvx_t + mvy_t * mvy_t + mvz_t * mvz_t);

    // 初值设 0 — 同轨道 S1 对 range 偏移在亚像素级
    // 旧公式 rangeOff = -|baseline|/rangeSpacing 高估 ~35px,
    // 导致近距采样点 slave 窗口越界被丢弃, 多项式拟合偏倚
    (void)nearRange; (void)centerCol; (void)rangeSpacing;
    rangeOff = 0.0;
    // 方位偏移 = 重采样源行偏移 (master 行 gRow ← slave 行 gRow + aziOff):
    // (s−m)·v̂ > 0 表示辅星超前 → 同一地物出现在辅影像更早的行 (gRow − |off|),
    // 故需取负号。2026-08 实测: 12 天对沿轨差 238 行时正号约定错 476 行,
    // 且 ESD 只能修 burst 间相对误差、无法修常数误差 → 输出整体错位。
    aziOff = -((sx_t - mx_t) * mvx_t + (sy_t - my_t) * mvy_t
             + (sz_t - mz_t) * mvz_t) / (mVmag * aziSpacing);
}
