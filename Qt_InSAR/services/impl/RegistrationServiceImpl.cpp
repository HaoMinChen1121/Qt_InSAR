#include "RegistrationServiceImpl.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalSlcWriter.h"
#include "dataaccess/SarProductFactory.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QScopedPointer>
#include <algorithm>
#include <numeric>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ──────────────────────────────────────────────────────────
// 数学辅助
// ──────────────────────────────────────────────────────────

namespace {

double sinc(double x) {
    if (std::abs(x) < 1e-8) return 1.0;
    return std::sin(M_PI * x) / (M_PI * x);
}

// 修正零阶贝塞尔函数 (多项式近似)
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

// 三次样条插值（自然边界）
struct CubicSpline {
    QVector<double> t, y, a, b, c, d;
    int n;

    CubicSpline(const QVector<double>& times, const QVector<double>& values) {
        n = times.size();
        if (n < 2) return;
        t = times;
        y = values;
        a.resize(n); b.resize(n); c.resize(n); d.resize(n);

        QVector<double> h(n - 1), alpha(n - 1);
        for (int i = 0; i < n - 1; ++i) {
            h[i] = t[i + 1] - t[i];
            if (h[i] > 0) {
                a[i] = y[i];
                alpha[i] = (3.0 / h[i]) * (y[i + 1] - y[i]) - (3.0 / (i > 0 ? h[i - 1] : h[i])) * (y[i] - y[i - 1]);
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
        for (int j = 0; j < n - 1; ++j) {
            if (x >= t[j] && x <= t[j + 1]) { i = j; break; }
        }
        double dx = x - t[i];
        return a[i] + b[i] * dx + c[i] * dx * dx + d[i] * dx * dx * dx;
    }
};

// 在轨道向量列表中插值位置
void interpolateOrbit(const QList<OrbitStateVector>& orbits,
                      double t, double& x, double& y, double& z,
                      double& vx, double& vy, double& vz)
{
    int n = orbits.size();
    if (n < 2) { x = y = z = vx = vy = vz = 0; return; }

    QVector<double> times(n);
    QVector<double> xs(n), ys(n), zs(n);
    QVector<double> vxs(n), vys(n), vzs(n);

    for (int i = 0; i < n; ++i) {
        times[i] = orbits[i].time;
        xs[i] = orbits[i].x;
        ys[i] = orbits[i].y;
        zs[i] = orbits[i].z;
        vxs[i] = orbits[i].vx;
        vys[i] = orbits[i].vy;
        vzs[i] = orbits[i].vz;
    }

    CubicSpline sx(times, xs), sy(times, ys), sz(times, zs);
    CubicSpline svx(times, vxs), svy(times, vys), svz(times, vzs);

    x = sx.eval(t); y = sy.eval(t); z = sz.eval(t);
    vx = svx.eval(t); vy = svy.eval(t); vz = svz.eval(t);
}

// 求解 6×6 线性方程组 (高斯消元)
bool solve6x6(double A[6][6], double b[6], double x[6]) {
    double aug[6][7];
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) aug[i][j] = A[i][j];
        aug[i][6] = b[i];
    }

    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        double maxVal = std::abs(aug[col][col]);
        for (int row = col + 1; row < 6; ++row) {
            if (std::abs(aug[row][col]) > maxVal) {
                maxVal = std::abs(aug[row][col]);
                pivot = row;
            }
        }
        if (maxVal < 1e-15) return false;

        if (pivot != col)
            for (int j = 0; j <= 6; ++j)
                std::swap(aug[col][j], aug[pivot][j]);

        double piv = aug[col][col];
        for (int j = col; j <= 6; ++j) aug[col][j] /= piv;
        for (int row = 0; row < 6; ++row) {
            if (row == col) continue;
            double factor = aug[row][col];
            for (int j = col; j <= 6; ++j)
                aug[row][j] -= factor * aug[col][j];
        }
    }
    for (int i = 0; i < 6; ++i) x[i] = aug[i][6];
    return true;
}

// 2D 抛物线插值 — 亚像素峰值
void subpixelPeak(const QVector<double>& corr, int corrW, int corrH,
                  int peakC, int peakR,
                  double& subC, double& subR) {
    if (peakC <= 0 || peakC >= corrW - 1 || peakR <= 0 || peakR >= corrH - 1) {
        subC = peakC; subR = peakR; return;
    }
    auto val = [&](int c, int r) { return corr[r * corrW + c]; };
    double f00 = val(peakC, peakR);
    double fm1 = val(peakC - 1, peakR);
    double fp1 = val(peakC + 1, peakR);
    double f0m1 = val(peakC, peakR - 1);
    double f0p1 = val(peakC, peakR + 1);

    double denomC = 2.0 * (2.0 * f00 - fm1 - fp1);
    subC = (denomC != 0) ? peakC + (fp1 - fm1) / denomC : peakC;

    double denomR = 2.0 * (2.0 * f00 - f0m1 - f0p1);
    subR = (denomR != 0) ? peakR + (f0p1 - f0m1) / denomR : peakR;

    subC = qBound(0.0, subC, double(corrW - 1));
    subR = qBound(0.0, subR, double(corrH - 1));
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────
// RegistrationServiceImpl
// ──────────────────────────────────────────────────────────

RegistrationServiceImpl::RegistrationServiceImpl(QObject* parent)
    : IRegistrationService(parent) {}

void RegistrationServiceImpl::setParams(const RegistrationParams& p) { mParams = p; }
RegistrationParams RegistrationServiceImpl::params() const { return mParams; }
double RegistrationServiceImpl::currentCorrelation() const { return mCorrelation; }
void RegistrationServiceImpl::cancel() { mCancelled = true; }
bool RegistrationServiceImpl::isRunning() const { return mRunning; }

// ──────────────────────────────────────────────────────────
// 主执行流程
// ──────────────────────────────────────────────────────────

void RegistrationServiceImpl::execute() {
    mRunning = true;
    mCancelled = false;
    mCorrelation = 0.0;

    // 在 WorkerManager 子线程中重新初始化 GDAL 驱动
    GDALAllRegister();

    QString mProd = mParams.masterProductPath;
    QString sProd = mParams.slaveProductPath;
    if (mProd.isEmpty()) mProd = mParams.masterPath;
    if (sProd.isEmpty()) sProd = mParams.slavePath;

    // 1. 打开主辅产品，发现波段
    emit progressChanged(0, QStringLiteral("打开产品..."));
    QScopedPointer<ISarProduct> masterProduct(createSarProduct(mProd));
    QScopedPointer<ISarProduct> slaveProduct(createSarProduct(sProd));
    if (!masterProduct || !masterProduct->open(mProd)) {
        emit errorOccurred(QStringLiteral("无法打开主产品: %1").arg(mProd));
        emit finished(false, QString()); mRunning = false; return;
    }
    if (!slaveProduct || !slaveProduct->open(sProd)) {
        emit errorOccurred(QStringLiteral("无法打开辅产品: %1").arg(sProd));
        emit finished(false, QString()); mRunning = false; return;
    }

    const auto& mBands = masterProduct->bands();
    const auto& sBands = slaveProduct->bands();

    // 2. 按 subSwath + polarization 配对
    struct BandPair {
        SarBandDescriptor master, slave;
    };
    QVector<BandPair> pairs;
    for (const auto& mb : mBands) {
        for (const auto& sb : sBands) {
            if (mb.subSwath == sb.subSwath && mb.polarization == sb.polarization) {
                pairs.append({mb, sb});
                break;
            }
        }
    }

    if (pairs.isEmpty()) {
        emit errorOccurred(QStringLiteral("未找到可配对的波段"));
        emit finished(false, QString()); mRunning = false; return;
    }

    qDebug() << "[Reg] 波段配对:" << pairs.size() << "对";

    // 3. 基线估算 (仅一次，共用)
    if (mParams.estimateBaseline && !mParams.masterOrbitVectors.isEmpty()) {
        BaselineInfo bl = estimateBaseline(
            mParams.masterOrbitVectors, mParams.slaveOrbitVectors,
            mParams.wavelength, mParams.masterNearRange,
            mParams.masterRangeSpacing,
            pairs.first().master.rasterSize.width());
        qDebug() << QStringLiteral("[Reg] 基线 — Bperp:%1m Bpar:%2m ΔT:%3d")
            .arg(bl.perpendicular, 0, 'f', 1)
            .arg(bl.parallel, 0, 'f', 1)
            .arg(bl.temporal, 0, 'f', 1);
    }

    // 4. 逐波段对配准
    int succeeded = 0;
    QString lastOutput;
    for (int i = 0; i < pairs.size(); ++i) {
        if (mCancelled) break;
        int basePct = i * 100 / pairs.size();
        emit progressChanged(basePct,
            QStringLiteral("%1/%2: %3 %4")
                .arg(i + 1).arg(pairs.size())
                .arg(pairs[i].master.subSwath)
                .arg(pairs[i].master.polarization));

        if (processBandPair(pairs[i].master, pairs[i].slave,
                mParams.outputDir, mParams.outputPrefix, i, pairs.size())) {
            ++succeeded;
        }
    }

    if (succeeded > 0) {
        emit progressChanged(100, QStringLiteral("配准完成 (%1/%2对)")
            .arg(succeeded).arg(pairs.size()));
        emit finished(true, lastOutput);
    } else {
        emit errorOccurred(QStringLiteral("所有波段对配准失败"));
        emit finished(false, QString());
    }
    mRunning = false;
}

// ── 单波段对配准 ──
bool RegistrationServiceImpl::processBandPair(
    const SarBandDescriptor& masterBand,
    const SarBandDescriptor& slaveBand,
    const QString& outputDir, const QString& prefix,
    int pairIndex, int totalPairs)
{
    Q_UNUSED(totalPairs);
    QString mPath = masterBand.rasterPath;
    QString sPath = slaveBand.rasterPath;
    QString pairName = QStringLiteral("%1of%2_%3_%4")
        .arg(pairIndex + 1).arg(totalPairs)
        .arg(masterBand.subSwath).arg(masterBand.polarization);

    // 全程保持 GDAL 数据集打开，避免重复 VSI open/close
    emit progressChanged(0, pairName + QStringLiteral(": 打开数据..."));
    GdalSlcReader mReader, sReader;
    if (!mReader.open(mPath)) {
        qWarning() << "[Reg]" << pairName << ": FAIL master open";
        return false;
    }
    int mW = mReader.width(), mH = mReader.height();
    if (!sReader.open(sPath)) {
        qWarning() << "[Reg]" << pairName << ": FAIL slave open";
        return false;
    }
    int sW = sReader.width(), sH = sReader.height();
    qDebug() << "[Reg]" << pairName << ": master" << mW << "x" << mH
             << "slave" << sW << "x" << sH;

    // 轨道粗配准
    emit progressChanged(10, pairName + QStringLiteral(": 轨道粗配准..."));
    auto gcps = coarseByOrbit(
        mParams.masterOrbitVectors, mParams.slaveOrbitVectors,
        mParams.masterDoppler, mParams.slaveDoppler,
        mParams.masterNearRange, mParams.masterRangeSpacing,
        mParams.masterAzimuthSpacing, mParams.masterPrf,
        mW, mH, mParams.coarseControlPoints);

    if (mCancelled) return false;

    // 互相关精化 (复用已打开的 mReader/sReader)
    emit progressChanged(20, pairName + QStringLiteral(": 互相关..."));
    qDebug() << "[Reg]" << pairName << ": cross-correlation start,"
             << gcps.size() << "GCPs";
    coarseByCorrelation(gcps, &mReader, &sReader, mW, mH, sW, sH,
        mParams.fineWindowSize, mParams.coarseSearchWindow,
        mParams.correlationThreshold);
    qDebug() << "[Reg]" << pairName << ": cross-correlation done";

    // 过滤
    QVector<CoarseGcp> validGcps;
    for (const auto& g : gcps)
        if (g.correlation >= mParams.correlationThreshold)
            validGcps.append(g);
    if (validGcps.size() < 6) {
        qWarning() << "[Reg]" << pairName << ": GCP不足:" << validGcps.size();
        return false;
    }

    // 精配准 (复用已打开的 mReader/sReader)
    emit progressChanged(40, pairName + QStringLiteral(": 精配准..."));
    qDebug() << "[Reg]" << pairName << ": fine reg start";
    int oversample = (mParams.fineMethod == "Oversample") ? 16 : 64;
    RegPolynomial poly = fineRegister(validGcps, &mReader, &sReader,
        mW, mH, sW, sH,
        oversample, mParams.polynomialDegree, mParams.correlationThreshold,
        mParams.fineWindowSize);

    mCorrelation = meanCorrelation(validGcps);
    if (poly.validGcps < 6) {
        qWarning() << "[Reg]" << pairName << ": poly fit failed GCPs:" << poly.validGcps;
        return false;
    }
    if (mCancelled) return false;

    // 重采样 (复用已打开的 sReader)
    emit progressChanged(60, pairName + QStringLiteral(": 重采样..."));
    qDebug() << "[Reg]" << pairName << ": resample start";
    QString outPath = outputDir.isEmpty()
        ? QFileInfo(mPath).absolutePath() + "/" + prefix + "_" + pairName + "_reg.tif"
        : outputDir + "/" + prefix + "_" + pairName + "_reg.tif";

    GdalSlcWriter writer;
    bool ok = resampleImage(poly, &sReader, &writer,
        mW, mH, sW, sH,
        mParams.resamplingMethod, mParams.sincWindowSize, mParams.sincBeta,
        outPath);

    if (ok) {
        emit progressChanged(90, pairName + QStringLiteral(": 完成"));
        qDebug() << "[Reg]" << pairName
                 << QStringLiteral("OK RMSE r:%1 a:%2")
                    .arg(poly.rmseRange, 0, 'f', 3)
                    .arg(poly.rmseAzimuth, 0, 'f', 3);
    } else {
        qWarning() << "[Reg]" << pairName << ": resample FAILED";
    }
    return ok;
}

// ──────────────────────────────────────────────────────────
// [1] 基线估算
// ──────────────────────────────────────────────────────────

BaselineInfo RegistrationServiceImpl::estimateBaseline(
    const QList<OrbitStateVector>& masterOrbits,
    const QList<OrbitStateVector>& slaveOrbits,
    double wavelength, double nearRange, double rangeSpacing,
    int rangeSamples)
{
    BaselineInfo info;

    if (masterOrbits.size() < 2 || slaveOrbits.size() < 2)
        return info;

    // 取主影像中间时刻为参考
    double t0 = (masterOrbits.first().time + masterOrbits.last().time) * 0.5;

    double mx, my, mz, mvx, mvy, mvz;
    double sx, sy, sz, svx, svy, svz;
    interpolateOrbit(masterOrbits, t0, mx, my, mz, mvx, mvy, mvz);
    interpolateOrbit(slaveOrbits, t0, sx, sy, sz, svx, svy, svz);

    double bx = sx - mx, by = sy - my, bz = sz - mz;
    double B = std::sqrt(bx * bx + by * by + bz * bz);

    // LOS 近似方向: 右视, 从卫星位置指向地心方向的水平分量
    double satDist = std::sqrt(mx * mx + my * my + mz * mz);
    double losX = -mx / satDist;
    double losY = -my / satDist;
    double losZ = -mz / satDist;

    double Bpar = bx * losX + by * losY + bz * losZ;
    double Bperp = std::sqrt(std::max(0.0, B * B - Bpar * Bpar));

    double slantRange = nearRange + (rangeSamples / 2.0) * rangeSpacing;
    double theta = std::acos((mx * losX + my * losY + mz * losZ)
        / std::sqrt(mx * mx + my * my + mz * mz));

    info.perpendicular = Bperp;
    info.parallel = Bpar;
    info.slantRange = slantRange;
    info.ambiguityHeight = (Bperp > 0.01)
        ? (wavelength * slantRange * std::sin(theta)) / (2.0 * Bperp) : 0;
    info.incidenceAngle = theta * 180.0 / M_PI;

    // 时间基线
    double tSlave = (slaveOrbits.first().time + slaveOrbits.last().time) * 0.5;
    info.temporal = std::abs(tSlave - t0) / 86400.0;

    return info;
}

// ──────────────────────────────────────────────────────────
// [2] 粗配准 — 轨道法
// ──────────────────────────────────────────────────────────

QVector<RegistrationServiceImpl::CoarseGcp>
RegistrationServiceImpl::coarseByOrbit(
    const QList<OrbitStateVector>& mOrb,
    const QList<OrbitStateVector>& sOrb,
    const DopplerInfo& mDop, const DopplerInfo& sDop,
    double nearRange, double rangeSpacing, double aziSpacing,
    double prf, int width, int height, int numGcp)
{
    QVector<CoarseGcp> gcps;
    if (mOrb.size() < 2 || sOrb.size() < 2) {
        // 无轨道数据 → 零偏移
        int cols = static_cast<int>(std::sqrt(numGcp));
        int rows = numGcp / cols;
        for (int ri = 0; ri < rows; ++ri) {
            for (int ci = 0; ci < cols; ++ci) {
                CoarseGcp g;
                g.col = (ci + 1) * width  / (cols + 1);
                g.row = (ri + 1) * height / (rows + 1);
                g.rangeOff = 0; g.aziOff = 0; g.correlation = 0;
                gcps.append(g);
            }
        }
        return gcps;
    }

    double t0 = (mOrb.first().time + mOrb.last().time) * 0.5;
    double mx, my, mz, mvx, mvy, mvz;
    double sx, sy, sz, svx, svy, svz;
    interpolateOrbit(mOrb, t0, mx, my, mz, mvx, mvy, mvz);
    interpolateOrbit(sOrb, t0, sx, sy, sz, svx, svy, svz);

    // 主影像速度大小 (方位向)
    double mVmag = std::sqrt(mvx * mvx + mvy * mvy + mvz * mvz);
    double sVmag = std::sqrt(svx * svx + svy * svy + svz * svz);

    double slantRangeMid = nearRange + (width / 2.0) * rangeSpacing;
    double Vr = 299792458.0; // 光速

    int cols = static_cast<int>(std::sqrt(static_cast<double>(numGcp)));
    int rows = numGcp / cols;

    for (int ri = 0; ri < rows; ++ri) {
        for (int ci = 0; ci < cols; ++ci) {
            CoarseGcp g;
            g.col = (ci + 1) * width  / (cols + 1);
            g.row = (ri + 1) * height / (rows + 1);

            double R = nearRange + g.col * rangeSpacing;
            double tAzi = t0 + g.row / prf;

            // 主影像卫星位置在该方位时刻
            double mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t;
            interpolateOrbit(mOrb, tAzi, mx_t, my_t, mz_t, mvx_t, mvy_t, mvz_t);

            // 辅影像卫星位置在同一方位时刻
            double sx_t, sy_t, sz_t, svx_t, svy_t, svz_t;
            interpolateOrbit(sOrb, tAzi, sx_t, sy_t, sz_t, svx_t, svy_t, svz_t);

            // 基线引起的斜距差 → 距离向偏移
            double dR = std::sqrt(
                (sx_t - mx_t) * (sx_t - mx_t) +
                (sy_t - my_t) * (sy_t - my_t) +
                (sz_t - mz_t) * (sz_t - mz_t));
            double rangeOff = -dR / rangeSpacing; // 近似

            // 方位向偏移 (来自速度差 + 斜距差)
            double mPosAtS = t0 + g.row / prf;
            double mx_s, my_s, mz_s, vx1, vy1, vz1;
            double sx_s, sy_s, sz_s, vx2, vy2, vz2;
            interpolateOrbit(mOrb, mPosAtS, mx_s, my_s, mz_s, vx1, vy1, vz1);
            interpolateOrbit(sOrb, mPosAtS, sx_s, sy_s, sz_s, vx2, vy2, vz2);
            double aziOff = ((sx_s - mx_s) * mvx_t + (sy_s - my_s) * mvy_t
                + (sz_s - mz_s) * mvz_t) / (mVmag * aziSpacing);

            g.rangeOff = rangeOff;
            g.aziOff = aziOff;
            g.correlation = 0;
            gcps.append(g);
        }
    }

    return gcps;
}

// ──────────────────────────────────────────────────────────
// [3] 互相关粗配准
// ──────────────────────────────────────────────────────────

void RegistrationServiceImpl::coarseByCorrelation(
    QVector<CoarseGcp>& gcps,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    int masterW, int masterH, int slaveW, int slaveH,
    int windowSize, int searchWindow, double corrThreshold)
{
    Q_UNUSED(corrThreshold);
    Q_UNUSED(masterW); Q_UNUSED(masterH);
    Q_UNUSED(slaveW); Q_UNUSED(slaveH);

    for (auto& gcp : gcps) {
        if (mCancelled) break;

        int halfWin = windowSize / 2;
        int mX0 = gcp.col - halfWin;
        int mY0 = gcp.row - halfWin;

        auto mData = mReader->readBandWindow(0, mX0, mY0, windowSize, windowSize);

        if (mData.size() < windowSize * windowSize)
            continue;

        // 计算主影像窗口的幅度
        QVector<double> mAmp(windowSize * windowSize);
        double mMagSum = 0;
        for (int i = 0; i < windowSize * windowSize; ++i) {
            mAmp[i] = std::abs(mData[i]);
            mMagSum += mAmp[i] * mAmp[i];
        }
        double mMagNorm = std::sqrt(mMagSum);
        if (mMagNorm < 1e-10) continue;

        // 在辅影像上搜索
        int sCenterX = gcp.col + static_cast<int>(gcp.rangeOff);
        int sCenterY = gcp.row + static_cast<int>(gcp.aziOff);

        int searchHalf = searchWindow / 2 + windowSize / 2;
        int sX0 = sCenterX - searchHalf;
        int sY0 = sCenterY - searchHalf;
        int sW = searchWindow + windowSize;
        int sH = sW;

        auto sData = sReader->readBandWindow(0, sX0, sY0, sW, sH);

        if (sData.size() < sW * sH) continue;

        // 辅影像幅度图
        QVector<double> sAmp(sW * sH);
        for (int i = 0; i < sW * sH; ++i)
            sAmp[i] = std::abs(sData[i]);

        // 滑动窗口 NCC
        double bestNcc = -1;
        int bestDx = 0, bestDy = 0;
        int searchRange = searchWindow;

        for (int dy = -searchRange; dy <= searchRange; ++dy) {
            for (int dx = -searchRange; dx <= searchRange; ++dx) {
                int sOffX = searchHalf - halfWin + dx - (sCenterX - sX0);
                int sOffY = searchHalf - halfWin + dy - (sCenterY - sY0);

                if (sOffX < 0 || sOffY < 0
                    || sOffX + windowSize > sW || sOffY + windowSize > sH)
                    continue;

                double cross = 0, sMagSum = 0;
                for (int y = 0; y < windowSize; ++y) {
                    for (int x = 0; x < windowSize; ++x) {
                        double sv = sAmp[(sOffY + y) * sW + (sOffX + x)];
                        cross += mAmp[y * windowSize + x] * sv;
                        sMagSum += sv * sv;
                    }
                }
                double ncc = cross / (mMagNorm * std::sqrt(std::max(1e-15, sMagSum)));

                if (ncc > bestNcc) {
                    bestNcc = ncc;
                    bestDx = dx;
                    bestDy = dy;
                }
            }
        }

        gcp.rangeOff += bestDx;
        gcp.aziOff   += bestDy;
        gcp.correlation = bestNcc;
    }
}

// ──────────────────────────────────────────────────────────
// [4] 精配准 — 亚像素 + 多项式
// ──────────────────────────────────────────────────────────

RegistrationServiceImpl::RegPolynomial
RegistrationServiceImpl::fineRegister(
    QVector<CoarseGcp>& gcps,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    int masterW, int masterH, int slaveW, int slaveH,
    int oversampleFactor, int polyDegree, double corrThreshold,
    int windowSize)
{
    Q_UNUSED(oversampleFactor);
    Q_UNUSED(slaveW); Q_UNUSED(slaveH);

    int halfWin = windowSize / 2;

    for (auto& gcp : gcps) {
        if (mCancelled) break;
        if (gcp.correlation < corrThreshold) continue;

        int mX0 = gcp.col - halfWin;
        int mY0 = gcp.row - halfWin;

        auto mData = mReader->readBandWindow(0, mX0, mY0, windowSize, windowSize);
        if (mData.size() < windowSize * windowSize) continue;

        QVector<double> mAmp(windowSize * windowSize);
        double mMagSum = 0;
        for (int i = 0; i < windowSize * windowSize; ++i) {
            mAmp[i] = std::abs(mData[i]);
            mMagSum += mAmp[i] * mAmp[i];
        }
        double mMagNorm = std::sqrt(std::max(1e-15, mMagSum));

        int sCenterX = gcp.col + static_cast<int>(gcp.rangeOff);
        int sCenterY = gcp.row + static_cast<int>(gcp.aziOff);
        int subSearch = 3;
        int sW = windowSize + subSearch * 2;

        int sX0 = sCenterX - halfWin - subSearch;
        int sY0 = sCenterY - halfWin - subSearch;

        auto sData = sReader->readBandWindow(0, sX0, sY0, sW, sW);
        if (sData.size() < sW * sW) continue;

        QVector<double> corrMap((2 * subSearch + 1) * (2 * subSearch + 1));
        int bestDx = 0, bestDy = 0;
        double bestNcc = -1;

        for (int dy = -subSearch; dy <= subSearch; ++dy) {
            for (int dx = -subSearch; dx <= subSearch; ++dx) {
                int offX = subSearch + dx;
                int offY = subSearch + dy;
                double cross = 0, sMagSum = 0;
                for (int y = 0; y < windowSize; ++y) {
                    for (int x = 0; x < windowSize; ++x) {
                        double sv = std::abs(sData[(offY + y) * sW + (offX + x)]);
                        cross += mAmp[y * windowSize + x] * sv;
                        sMagSum += sv * sv;
                    }
                }
                double ncc = cross / (mMagNorm * std::sqrt(std::max(1e-15, sMagSum)));
                corrMap[(dy + subSearch) * (2 * subSearch + 1) + (dx + subSearch)] = ncc;
                if (ncc > bestNcc) { bestNcc = ncc; bestDx = dx; bestDy = dy; }
            }
        }

        double subDx, subDy;
        subpixelPeak(corrMap, 2 * subSearch + 1, 2 * subSearch + 1,
                     bestDx + subSearch, bestDy + subSearch, subDx, subDy);
        subDx -= subSearch;
        subDy -= subSearch;

        gcp.rangeOff += subDx;
        gcp.aziOff   += subDy;
        gcp.correlation = bestNcc;
    }

    // 过滤低质量 GCP + 迭代剔除异常值
    RegPolynomial poly = {};
    poly.rmseRange = poly.rmseAzimuth = 1e9;

    for (int iter = 0; iter < 3; ++iter) {
        QVector<CoarseGcp> active;
        for (const auto& g : gcps)
            if (g.correlation >= corrThreshold)
                active.append(g);
        if (active.size() < 6) break;

        int N = active.size();
        int degree = qBound(1, polyDegree, 3); // 实际只用2阶

        // 构建设计矩阵 for 2nd order (6 coeffs)
        double ATA[6][6] = {};
        double ATbR[6] = {};
        double ATbA[6] = {};

        for (const auto& g : active) {
            double r = static_cast<double>(g.col) / masterW; // 归一化
            double a = static_cast<double>(g.row) / masterH;
            double basis[6] = {1.0, r, a, r * a, r * r, a * a};

            for (int i = 0; i < 6; ++i) {
                for (int j = 0; j < 6; ++j)
                    ATA[i][j] += basis[i] * basis[j];
                ATbR[i] += basis[i] * g.rangeOff;
                ATbA[i] += basis[i] * g.aziOff;
            }
        }

        double rCoeffs[6], aCoeffs[6];
        if (!solve6x6(ATA, ATbR, rCoeffs) || !solve6x6(ATA, ATbA, aCoeffs))
            break;

        // 计算残差
        double sumSqR = 0, sumSqA = 0;
        QVector<double> residuals(active.size());
        for (int i = 0; i < N; ++i) {
            double r = static_cast<double>(active[i].col) / masterW;
            double a = static_cast<double>(active[i].row) / masterH;
            double predR = rCoeffs[0] + rCoeffs[1]*r + rCoeffs[2]*a
                + rCoeffs[3]*r*a + rCoeffs[4]*r*r + rCoeffs[5]*a*a;
            double predA = aCoeffs[0] + aCoeffs[1]*r + aCoeffs[2]*a
                + aCoeffs[3]*r*a + aCoeffs[4]*r*r + aCoeffs[5]*a*a;
            double resR = active[i].rangeOff - predR;
            double resA = active[i].aziOff - predA;
            residuals[i] = std::sqrt(resR * resR + resA * resA);
            sumSqR += resR * resR;
            sumSqA += resA * resA;
        }

        double rmseR = std::sqrt(sumSqR / N);
        double rmseA = std::sqrt(sumSqA / N);

        // 剔除 > 3σ 异常
        if (iter < 2) {
            double meanRes = 0, stdRes = 0;
            for (double v : residuals) meanRes += v;
            meanRes /= N;
            for (double v : residuals) stdRes += (v - meanRes) * (v - meanRes);
            stdRes = std::sqrt(stdRes / N);
            double thresh = meanRes + 3.0 * stdRes;
            for (int i = 0; i < active.size(); ++i) {
                if (residuals[i] > thresh)
                    gcps[i].correlation = 0; // 标记剔除
            }
        }

        if (iter == 2 || (rmseR < 0.1 && rmseA < 0.1)) {
            for (int i = 0; i < 6; ++i) {
                poly.rangeCoeffs[i] = rCoeffs[i];
                poly.aziCoeffs[i]   = aCoeffs[i];
            }
            poly.rmseRange   = rmseR;
            poly.rmseAzimuth = rmseA;
            poly.validGcps   = N;
        }
    }

    return poly;
}

// ──────────────────────────────────────────────────────────
// [5] 重采样
// ──────────────────────────────────────────────────────────

bool RegistrationServiceImpl::resampleImage(
    const RegPolynomial& poly,
    GdalSlcReader* sReader, GdalSlcWriter* writer,
    int masterW, int masterH, int slaveW, int slaveH,
    const QString& interpMethod, int sincWindow, double sincBeta,
    const QString& outputPath)
{
    qDebug() << "[Reg] resample: creating output" << outputPath;
    if (!writer->create(outputPath, masterW, masterH, 1)) {
        qWarning() << "[Reg] 创建输出文件失败:" << writer->lastError();
        return false;
    }

    // 逐行重采样 + 写入，避免分配 2GB 全图缓冲
    QVector<std::complex<float>> rowBuf(masterW);
    int step = qMax(1, masterH / 100);

    for (int row = 0; row < masterH; ++row) {
        if (mCancelled) return false;

        if (row % step == 0) {
            int pct = 60 + (row * 40 / masterH);
            emit progressChanged(pct, QStringLiteral("重采样 %1/%2").arg(row).arg(masterH));
        }

        double rNorm = 0.5;
        double aNorm = static_cast<double>(row) / masterH;
        double rowOff = poly.aziCoeffs[0] + poly.aziCoeffs[1]*rNorm + poly.aziCoeffs[2]*aNorm
            + poly.aziCoeffs[3]*rNorm*aNorm + poly.aziCoeffs[4]*rNorm*rNorm + poly.aziCoeffs[5]*aNorm*aNorm;
        int slaveRow = row + static_cast<int>(rowOff);

        int readRadius = (interpMethod == "Sinc") ? sincWindow : 2;
        int sY0 = slaveRow - readRadius;
        int sYH = readRadius * 2 + 1;
        if (sY0 < 0) { sYH += sY0; sY0 = 0; }
        if (sY0 + sYH > slaveH) sYH = slaveH - sY0;
        if (sYH <= 0) {
            rowBuf.fill(std::complex<float>(0, 0));
        } else {
            auto sWindow = sReader->readBandWindow(0, 0, sY0, slaveW, sYH);
            for (int col = 0; col < masterW; ++col) {
                double rN = static_cast<double>(col) / masterW;
                double aN = static_cast<double>(row) / masterH;
                double colOff = poly.rangeCoeffs[0] + poly.rangeCoeffs[1]*rN + poly.rangeCoeffs[2]*aN
                    + poly.rangeCoeffs[3]*rN*aN + poly.rangeCoeffs[4]*rN*rN + poly.rangeCoeffs[5]*aN*aN;

                double sx = col + colOff;
                double sy = static_cast<double>(row + static_cast<int>(rowOff) - sY0
                    + (rowOff - static_cast<int>(rowOff)));

                if (sx >= 0 && sx < slaveW - 1 && sy >= 0 && sy < sYH - 1
                    && sWindow.size() >= slaveW * sYH) {
                    rowBuf[col] = interp2D(sWindow, slaveW, sYH, sx, sy,
                        interpMethod, sincWindow, sincBeta);
                } else {
                    rowBuf[col] = std::complex<float>(0, 0);
                }
            }
        }

        // 逐行写入
        writer->writeRow(row, rowBuf);
    }

    return true;
}

// ──────────────────────────────────────────────────────────
// 2D 复数插值
// ──────────────────────────────────────────────────────────

std::complex<float> RegistrationServiceImpl::interp2D(
    const QVector<std::complex<float>>& data,
    int width, int height, double x, double y,
    const QString& method, int sincWindow, double sincBeta)
{
    if (method == "Bilinear") {
        int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        int x1 = x0 + 1, y1 = y0 + 1;
        x0 = qBound(0, x0, width - 1);  x1 = qBound(0, x1, width - 1);
        y0 = qBound(0, y0, height - 1); y1 = qBound(0, y1, height - 1);

        double fx = x - std::floor(x), fy = y - std::floor(y);

        auto f00 = data[y0 * width + x0]; auto f10 = data[y0 * width + x1];
        auto f01 = data[y1 * width + x0]; auto f11 = data[y1 * width + x1];

        float r = (1-fx)*(1-fy)*f00.real() + fx*(1-fy)*f10.real()
            + (1-fx)*fy*f01.real() + fx*fy*f11.real();
        float im = (1-fx)*(1-fy)*f00.imag() + fx*(1-fy)*f10.imag()
            + (1-fx)*fy*f01.imag() + fx*fy*f11.imag();
        return {r, im};
    }

    if (method == "Bicubic") {
        int ix = static_cast<int>(std::floor(x));
        int iy = static_cast<int>(std::floor(y));
        double fx = x - ix, fy = y - iy;

        // 对4列分别沿y方向插值
        double colValsR[4], colValsI[4];
        for (int c = -1; c <= 2; ++c) {
            int cx = qBound(0, ix + c, width - 1);
            double rowValsR[4], rowValsI[4];
            for (int r = -1; r <= 2; ++r) {
                int ry = qBound(0, iy + r, height - 1);
                auto v = data[ry * width + cx];
                rowValsR[r + 1] = v.real();
                rowValsI[r + 1] = v.imag();
            }
            // Catmull-Rom 沿y
            auto cubic4 = [](double t, double vm1, double v0, double v1, double v2) {
                return v0 + 0.5*t*(v1 - vm1 + t*(2.0*vm1 - 5.0*v0 + 4.0*v1 - v2
                    + t*(3.0*(v0 - v1) + v2 - vm1)));
            };
            colValsR[c + 1] = cubic4(fy, rowValsR[0], rowValsR[1], rowValsR[2], rowValsR[3]);
            colValsI[c + 1] = cubic4(fy, rowValsI[0], rowValsI[1], rowValsI[2], rowValsI[3]);
        }
        auto cubic4 = [](double t, double vm1, double v0, double v1, double v2) {
            return v0 + 0.5*t*(v1 - vm1 + t*(2.0*vm1 - 5.0*v0 + 4.0*v1 - v2
                + t*(3.0*(v0 - v1) + v2 - vm1)));
        };
        float r = static_cast<float>(cubic4(fx, colValsR[0], colValsR[1], colValsR[2], colValsR[3]));
        float im = static_cast<float>(cubic4(fx, colValsI[0], colValsI[1], colValsI[2], colValsI[3]));
        return {r, im};
    }

    // Sinc (default)
    {
        int ix = static_cast<int>(std::floor(x));
        int iy = static_cast<int>(std::floor(y));
        double fx = x - ix, fy = y - iy;

        double resultR = 0, resultI = 0;
        double weightSum = 0;

        for (int c = -sincWindow; c <= sincWindow; ++c) {
            int cx = qBound(0, ix + c, width - 1);
            double wx = sinc(c - fx) * kaiserWindow(
                c + sincWindow, 2 * sincWindow + 1, sincBeta);

            double colR = 0, colI = 0, colWSum = 0;
            for (int r = -sincWindow; r <= sincWindow; ++r) {
                int ry = qBound(0, iy + r, height - 1);
                double wy = sinc(r - fy) * kaiserWindow(
                    r + sincWindow, 2 * sincWindow + 1, sincBeta);
                auto v = data[ry * width + cx];
                double w = wx * wy;
                colR += v.real() * w;
                colI += v.imag() * w;
                colWSum += w;
            }
            if (colWSum > 0) { colR /= colWSum; colI /= colWSum; }
            resultR += colR;
            resultI += colI;
            weightSum += 1;
        }

        if (weightSum > 0) { resultR /= weightSum; resultI /= weightSum; }
        return {static_cast<float>(resultR), static_cast<float>(resultI)};
    }
}

double RegistrationServiceImpl::meanCorrelation(const QVector<CoarseGcp>& gcps) const {
    if (gcps.isEmpty()) return 0;
    double sum = 0; int cnt = 0;
    for (const auto& g : gcps)
        if (g.correlation > 0) { sum += g.correlation; ++cnt; }
    return cnt > 0 ? sum / cnt : 0;
}
