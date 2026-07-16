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
                alpha[i] = (3.0 / h[i]) * (y[i + 1] - y[i])
                    - (3.0 / (i > 0 ? h[i - 1] : h[i])) * (y[i] - y[i - 1]);
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
        times[i] = orbits[i].time;
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
    double t0 = (mOrb.first().time + mOrb.last().time) * 0.5;
    double tAzi = t0 + centerRow / prf;
    double R = nearRange + centerCol * rangeSpacing;

    double mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t;
    double sx_t, sy_t, sz_t, svx_t, svy_t, svz_t;
    interpolateOrbit(mOrb, tAzi, mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t);
    interpolateOrbit(sOrb, tAzi, sx_t, sy_t, sz_t, svx_t, svy_t, svz_t);

    double mVmag = std::sqrt(mvx_t * mvx_t + mvy_t * mvy_t + mvz_t * mvz_t);

    double dR = std::sqrt((sx_t - mx_t) * (sx_t - mx_t)
                        + (sy_t - my_t) * (sy_t - my_t)
                        + (sz_t - mz_t) * (sz_t - mz_t));
    rangeOff = -dR / rangeSpacing;
    aziOff = ((sx_t - mx_t) * mvx_t + (sy_t - my_t) * mvy_t
            + (sz_t - mz_t) * mvz_t) / (mVmag * aziSpacing);
}
