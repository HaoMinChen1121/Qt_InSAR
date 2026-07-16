#include "RegistrationServiceImpl.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalSlcWriter.h"
#include "dataaccess/SarProductFactory.h"
#include "dataaccess/impl/QsarIO.h"
#include "domain/QsarProduct.h"

#include <gdal_priv.h>

#include "algorithms/Correlation.h"

#include <QtMath>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QScopedPointer>
#include <QApplication>
#include <QDateTime>
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

// 2 阶多项式求值: c0 + c1*r + c2*a + c3*r*a + c4*r² + c5*a²
inline double evalPoly(const double c[6], double r, double a) {
    return c[0] + c[1]*r + c[2]*a + c[3]*r*a + c[4]*r*r + c[5]*a*a;
}

// 从辅影像显示名提取日期前缀 (e.g. "S1A_0617 Orbit83" → "0617_<prefix>")
inline QString makeDatePrefix(const QString& slaveDisplayName, const QString& outputPrefix) {
    if (!slaveDisplayName.isEmpty()) {
        QStringList parts = slaveDisplayName.split('_');
        if (parts.size() >= 2)
            return parts[1] + "_" + outputPrefix;
    }
    return outputPrefix;
}

// 构造配准输出路径
inline QString makeOutputPath(const QString& outputDir, const QString& datePrefix,
                               const QString& pairName) {
    QString dir = outputDir.isEmpty() ? QDir::tempPath() : outputDir;
    return dir + "/" + datePrefix + "_" + pairName + "_reg.tif";
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
                mParams.outputDir, mParams.outputPrefix, i)) {
            ++succeeded;
        }
    }

    if (succeeded > 0) {
        // 写出 .qsar 产品描述头文件
        QsarProduct qsar;
        qsar.productType = "RegisteredSLC";
        qsar.created = QDateTime::currentDateTime().toString(Qt::ISODate);
        qsar.sourceMaster = mParams.masterDisplayName;
        qsar.sourceSlave = mParams.slaveDisplayName;
        qsar.coarseMethod = mParams.coarseMethod;
        qsar.resamplingMethod = mParams.resamplingMethod;
        qsar.outputPrefix = mParams.outputPrefix;
        qsar.baseline.perpendicular = 0;
        qsar.baseline.temporal = 0;
        QString datePrefix = makeDatePrefix(mParams.slaveDisplayName, mParams.outputPrefix);

        QString qsarDir;
        for (int i = 0; i < pairs.size(); ++i) {
            QsarBand b;
            b.subSwath = pairs[i].master.subSwath;
            b.polarization = pairs[i].master.polarization;
            b.width = pairs[i].master.rasterSize.width();
            b.height = pairs[i].master.rasterSize.height();
            QString pairName = QStringLiteral("%1_%2_%3")
                .arg(i + 1)
                .arg(b.subSwath).arg(b.polarization);
            QString outPath = makeOutputPath(mParams.outputDir, datePrefix, pairName);
            b.file = QFileInfo(outPath).fileName();
            qsar.bands.append(b);
            qsarDir = QFileInfo(outPath).absolutePath();
        }
        if (!qsarDir.isEmpty()) {
            QString qsarPath = qsarDir + "/" + datePrefix + ".qsar";
            QsarIO::write(qsarPath, qsar);
            lastOutput = qsarPath;
        }

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
    int pairIndex)
{
    QString mPath = masterBand.rasterPath;
    QString sPath = slaveBand.rasterPath;
    QString pairName = QStringLiteral("%1_%2_%3")
        .arg(pairIndex + 1)
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

    // 粗配准 — NCC 或 FFT
    if (mParams.coarseMethod == "FFT") {
        emit progressChanged(20, pairName + QStringLiteral(": FFT粗配准..."));
        coarseByFFT(gcps, &mReader, &sReader, mW, mH, sW, sH, mParams.coarseWindowSize);
    } else {
        emit progressChanged(20, pairName + QStringLiteral(": 互相关..."));
        coarseByCorrelation(gcps, &mReader, &sReader,
            mParams.fineWindowSize, mParams.coarseSearchWindow);
    }

    // 过滤
    QVector<CoarseGcp> validGcps;
    for (const auto& g : gcps)
        if (g.correlation >= mParams.correlationThreshold)
            validGcps.append(g);
    if (validGcps.size() < 6) {
        qWarning() << "[Reg]" << pairName << ": GCP不足:" << validGcps.size();
        return false;
    }

    // FFTW3 复数域精配准 (可选)
    if (mParams.enableFineFFT) {
        emit progressChanged(35, pairName + QStringLiteral(": FFT精配准..."));
        fftFineRefine(validGcps, &mReader, &sReader, mW, mH, sW, sH, mParams.fineFFTWindow);
    }

    // 精配准 (复用已打开的 mReader/sReader)
    emit progressChanged(40, pairName + QStringLiteral(": 精配准..."));
    qDebug() << "[Reg]" << pairName << ": fine reg start";
    RegPolynomial poly = fineRegister(validGcps, &mReader, &sReader,
        mW, mH, mParams.correlationThreshold,
        mParams.fineWindowSize);

    mCorrelation = meanCorrelation(validGcps);
    if (poly.validGcps < 6) {
        qWarning() << "[Reg]" << pairName << ": poly fit failed GCPs:" << poly.validGcps;
        return false;
    }
    if (mCancelled) return false;

    // ESD 方位向精化 (TOPSAR burst overlap spectral diversity)
    if (mParams.enableEsd && masterBand.burstCount > 1) {
        emit progressChanged(50, pairName + QStringLiteral(": ESD精化..."));
        qDebug() << "[Reg]" << pairName << ": ESD start, bursts:" << masterBand.burstCount;
        esdRefine(poly, &mReader, &sReader, masterBand, slaveBand,
                  mW, mH);
        qDebug() << "[Reg]" << pairName << ": ESD done";
    }

    // 重采样 (复用已打开的 sReader)
    emit progressChanged(60, pairName + QStringLiteral(": 重采样..."));
    qDebug() << "[Reg]" << pairName << ": resample start";
    if (outputDir.isEmpty()) {
        qWarning() << "[Reg] outputDir not set, using temp path";
    }
    QString datePrefix = makeDatePrefix(mParams.slaveDisplayName, prefix);
    QString outPath = makeOutputPath(outputDir, datePrefix, pairName);

    GdalSlcWriter writer;
    bool ok;

    // TOPSAR 多 burst 数据: 逐 burst 独立配准 + 拼接
    if (masterBand.burstCount > 1 && masterBand.linesPerBurst > 100) {
        emit progressChanged(62, pairName + QStringLiteral(": 逐burst拟合..."));
        auto burstPolys = fitBurstLocalPolynomials(
            validGcps, poly, masterBand, mW, mH);
        qDebug() << "[Reg]" << pairName << ": per-burst resample,"
                 << burstPolys.size() << "bursts";
        ok = resampleImagePerBurst(burstPolys, &sReader, &writer,
            mW, mH, sW, sH,
            mParams.resamplingMethod, mParams.sincWindowSize, mParams.sincBeta,
            outPath);
    } else {
        ok = resampleImage(poly, &sReader, &writer,
            mW, mH, sW, sH,
            mParams.resamplingMethod, mParams.sincWindowSize, mParams.sincBeta,
            outPath);
    }

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
    int windowSize, int searchWindow)
{

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
// ──────────────────────────────────────────────────────────
// [New] FFTW3 幅度域粗配准
// ──────────────────────────────────────────────────────────
void RegistrationServiceImpl::coarseByFFT(
    QVector<CoarseGcp>& gcps,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    int masterW, int masterH, int slaveW, int slaveH,
    int windowSize)
{
    int half = windowSize / 2;
    for (auto& gcp : gcps) {
        if (mCancelled) break;
        int mX0 = gcp.col - half, mY0 = gcp.row - half;
        auto mWin = mReader->readBandWindow(0, mX0, mY0, windowSize, windowSize);
        if (mWin.size() < windowSize * windowSize) continue;

        int sX0 = gcp.col + (int)gcp.rangeOff - half;
        int sY0 = gcp.row + (int)gcp.aziOff - half;
        if (sX0 < 0 || sY0 < 0 || sX0 + windowSize > slaveW || sY0 + windowSize > slaveH) continue;
        auto sWin = sReader->readBandWindow(0, sX0, sY0, windowSize, windowSize);
        if (sWin.size() < windowSize * windowSize) continue;

        int oR = 2 * windowSize - 1, oC = 2 * windowSize - 1;
        QVector<float> surf(oR * oC);
        float maxV = fftAmpCorrelate(mWin.data(), sWin.data(), surf.data(), windowSize, windowSize);
        if (maxV > 0) {
            double sDx, sDy;
            findPeakSubpixel(surf.data(), oR, oC, sDx, sDy);
            gcp.rangeOff += sDx;
            gcp.aziOff   += sDy;
            gcp.correlation = maxV;
        }
    }
}

// ──────────────────────────────────────────────────────────
// [New] FFTW3 复数域精配准
// ──────────────────────────────────────────────────────────
void RegistrationServiceImpl::fftFineRefine(
    QVector<CoarseGcp>& gcps,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    int masterW, int masterH, int slaveW, int slaveH,
    int windowSize)
{
    int half = windowSize / 2;
    for (auto& gcp : gcps) {
        if (mCancelled) break;
        if (gcp.correlation < mParams.correlationThreshold) continue;

        int mX0 = gcp.col - half, mY0 = gcp.row - half;
        auto mWin = mReader->readBandWindow(0, mX0, mY0, windowSize, windowSize);
        if (mWin.size() < windowSize * windowSize) continue;

        int sX0 = gcp.col + (int)gcp.rangeOff - half;
        int sY0 = gcp.row + (int)gcp.aziOff - half;
        if (sX0 < 0 || sY0 < 0 || sX0 + windowSize > slaveW || sY0 + windowSize > slaveH) continue;
        auto sWin = sReader->readBandWindow(0, sX0, sY0, windowSize, windowSize);
        if (sWin.size() < windowSize * windowSize) continue;

        int oR = 2 * windowSize - 1, oC = 2 * windowSize - 1;
        QVector<float> surf(oR * oC);
        fftPhaseCorrelate(mWin.data(), sWin.data(), surf.data(), windowSize, windowSize);
        double sDx, sDy;
        findPeakSubpixel(surf.data(), oR, oC, sDx, sDy);
        gcp.rangeOff += sDx;
        gcp.aziOff   += sDy;
    }
    double c = meanCorrelation(gcps);
    qDebug() << "[Reg] FFT fine refined" << gcps.size() << "GCPs, meanCorr:" << c;
}

// ──────────────────────────────────────────────────────────
// ──────────────────────────────────────────────────────────
// ESD — 增强频谱分集方位向精化 (TOPSAR)
// ──────────────────────────────────────────────────────────
void RegistrationServiceImpl::esdRefine(
    RegPolynomial& poly,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    const SarBandDescriptor& masterBand,
    const SarBandDescriptor& slaveBand,
    int masterW, int masterH)
{

    int burstCount = masterBand.burstCount;
    int linesPerBurst = masterBand.linesPerBurst;
    const auto& burstStarts = masterBand.burstStartLines;

    if (burstCount < 2 || burstStarts.size() < 2 || linesPerBurst < 100)
        return;

    // Doppler 差: TOPSAR 典型值 ~5000 Hz
    const double deltaFdoppler = 5000.0;
    const int overlapLines = qMin(100, linesPerBurst / 10);

    QVector<double> aziCorrections; // 每个 burst 的方位向修正 (像素)
    aziCorrections.append(0.0);     // 第一个 burst 作为参考

    int halfWin = 16; // 互相关窗口的一半

    for (int b = 1; b < burstCount; ++b) {
        if (mCancelled) return;

        // burst b-1 和 b 的重叠区中心行
        int lineA = burstStarts[b - 1] + linesPerBurst - overlapLines / 2;
        int lineB = burstStarts[b] + overlapLines / 2;

        // 确保在有效范围内
        lineA = qBound(halfWin, lineA, masterH - halfWin);
        lineB = qBound(halfWin, lineB, masterH - halfWin);

        // 读取重叠区窗口 (range方向取中心一半，方位向取 overlap)
        int col0 = masterW / 4;
        int colW = masterW / 2;
        int rowH = overlapLines;

        auto mWinA = mReader->readBandWindow(0, col0, lineA - rowH / 2, colW, rowH);
        auto sWinA = sReader->readBandWindow(0, col0, lineA - rowH / 2, colW, rowH);
        auto mWinB = mReader->readBandWindow(0, col0, lineB - rowH / 2, colW, rowH);
        auto sWinB = sReader->readBandWindow(0, col0, lineB - rowH / 2, colW, rowH);

        if (mWinA.size() < colW * rowH || sWinA.size() < colW * rowH
            || mWinB.size() < colW * rowH || sWinB.size() < colW * rowH) {
            aziCorrections.append(0.0);
            continue;
        }

        // 计算干涉图 (两个 burst 的重叠区)
        std::complex<double> esdSum(0, 0);
        for (int k = 0; k < colW * rowH; ++k) {
            std::complex<double> ifgA = std::complex<double>(mWinA[k].real(), mWinA[k].imag())
                * std::complex<double>(sWinA[k].real(), -sWinA[k].imag());
            std::complex<double> ifgB = std::complex<double>(mWinB[k].real(), mWinB[k].imag())
                * std::complex<double>(sWinB[k].real(), -sWinB[k].imag());
            // ESD: ifgA * conj(ifgB)
            esdSum += ifgA * std::conj(ifgB);
        }

        double esdPhase = std::arg(esdSum);
        // 残余方位偏移: Δaz = φ / (2π * Δf_doppler)
        // 转换为像素: offset = Δaz * azSpacing → 此处用直接像素修正
        double aziCorr = esdPhase / (2.0 * M_PI * deltaFdoppler / mParams.masterPrf);
        aziCorrections.append(aziCorr);
    }

    // 将 burst 级修正保存到多项式 (不再压缩为均值)
    if (aziCorrections.size() < 2) return;

    // 相对修正 → 累积绝对修正
    QVector<double> absCorr(burstCount);
    absCorr[0] = 0.0;
    for (int b = 1; b < burstCount; ++b) {
        absCorr[b] = absCorr[b - 1] + aziCorrections[b];
    }

    // 滤除异常值
    for (int b = 0; b < burstCount; ++b) {
        if (std::abs(absCorr[b]) >= 1.0)
            absCorr[b] = 0.0;
    }

    // 中心化: 分离均值 (归入多项式) 与逐 burst 残差
    double sum = 0;
    int validCount = 0;
    for (int b = 0; b < burstCount; ++b) {
        sum += absCorr[b];
        ++validCount;
    }
    if (validCount > 0) {
        double meanCorr = sum / validCount;
        poly.aziCoeffs[0] += meanCorr;
        for (int b = 0; b < burstCount; ++b)
            absCorr[b] -= meanCorr;
    }

    poly.burstAziCorrections = absCorr;
    poly.burstStartLines     = burstStarts;
    poly.linesPerBurst       = linesPerBurst;

    // 诊断 RMS
    double sqSum = 0;
    for (int b = 0; b < burstCount; ++b)
        sqSum += absCorr[b] * absCorr[b];
    poly.rmseAzimuth = std::sqrt(sqSum / burstCount);

    qDebug() << "[Reg] ESD: per-burst corrections stored, RMSE ="
             << poly.rmseAzimuth << "pix";
}

// ──────────────────────────────────────────────────────────
// 逐 burst 局部多项式拟合 (TOPSAR)
// ──────────────────────────────────────────────────────────

QVector<RegistrationServiceImpl::BurstLocalPoly>
RegistrationServiceImpl::fitBurstLocalPolynomials(
    const QVector<CoarseGcp>& gcps,
    const RegPolynomial& globalPoly,
    const SarBandDescriptor& masterBand,
    int masterW, int masterH)
{
    QVector<BurstLocalPoly> result;

    int burstCount = masterBand.burstCount;
    int linesPerBurst = masterBand.linesPerBurst;
    const auto& burstStarts = masterBand.burstStartLines;

    // 退化情况: 单 burst 或无 burst 信息, 回退到全局多项式
    if (burstCount < 2 || burstStarts.size() < burstCount || linesPerBurst < 100) {
        BurstLocalPoly bp;
        for (int i = 0; i < 6; ++i) {
            bp.rangeCoeffs[i] = globalPoly.rangeCoeffs[i];
            bp.aziCoeffs[i]   = globalPoly.aziCoeffs[i];
        }
        bp.masterStartLine = 0;
        bp.masterEndLine   = masterH;
        result.append(bp);
        return result;
    }

    // 每个 burst 的 padding: 向相邻 burst 扩展以包含重叠区 GCP
    int padding = qMin(linesPerBurst / 10, 100);

    for (int b = 0; b < burstCount; ++b) {
        BurstLocalPoly bp;
        bp.masterStartLine = burstStarts[b];
        bp.masterEndLine   = burstStarts[b] + linesPerBurst;

        // 截断到影像范围
        bp.masterStartLine = qMax(0, bp.masterStartLine);
        bp.masterEndLine   = qMin(masterH, bp.masterEndLine);

        int gcpsStart = bp.masterStartLine - padding;
        int gcpsEnd   = bp.masterEndLine + padding;

        QVector<CoarseGcp> burstGcps;
        for (const auto& g : gcps) {
            if (g.correlation >= mParams.correlationThreshold
                && g.row >= gcpsStart && g.row < gcpsEnd) {
                burstGcps.append(g);
            }
        }

        // 需要 ≥8 个 GCP 才拟合局部多项式 (比全局 6 个更严格以抑制过拟合)
        bool localOk = burstGcps.size() >= 8
            && fitPolynomial(burstGcps, masterW, masterH,
                             bp.rangeCoeffs, bp.aziCoeffs);

        if (localOk) {
            // 局部多项式拟合成功: 叠加上该 burst 的 ESD 残余修正
            if (b < globalPoly.burstAziCorrections.size()) {
                bp.aziCoeffs[0] += globalPoly.burstAziCorrections[b];
            }
        } else {
            // 回退: 使用全局多项式 + ESD 修正
            for (int i = 0; i < 6; ++i) {
                bp.rangeCoeffs[i] = globalPoly.rangeCoeffs[i];
                bp.aziCoeffs[i]   = globalPoly.aziCoeffs[i];
            }
            if (b < globalPoly.burstAziCorrections.size()) {
                bp.aziCoeffs[0] += globalPoly.burstAziCorrections[b];
            }
        }

        result.append(bp);
    }

    return result;
}

// [4] 精配准 — 亚像素 + 多项式
// ──────────────────────────────────────────────────────────

RegistrationServiceImpl::RegPolynomial
RegistrationServiceImpl::fineRegister(
    QVector<CoarseGcp>& gcps,
    GdalSlcReader* mReader, GdalSlcReader* sReader,
    int masterW, int masterH, double corrThreshold,
    int windowSize)
{

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

        double rCoeffs[6], aCoeffs[6];
        if (!fitPolynomial(active, masterW, masterH, rCoeffs, aCoeffs))
            break;

        // 计算残差
        double sumSqR = 0, sumSqA = 0;
        QVector<double> residuals(active.size());
        for (int i = 0; i < N; ++i) {
            double r = static_cast<double>(active[i].col) / masterW;
            double a = static_cast<double>(active[i].row) / masterH;
            double predR = evalPoly(rCoeffs, r, a);
            double predA = evalPoly(aCoeffs, r, a);
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

    // 设置 geotransform（从主影像输入复制，无地理参考则用默认像素坐标）
    writer->setGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    // 逐行重采样 + 写入，避免分配 2GB 全图缓冲
    QVector<std::complex<float>> rowBuf(masterW);
    int step = qMax(1, masterH / 100);
    double aNormRow, rowOffRow, syBase;
    int slaveRowBase;

    for (int row = 0; row < masterH; ++row) {
        if (mCancelled) return false;

        if (row % step == 0) {
            int pct = 60 + (row * 40 / masterH);
            emit progressChanged(pct, QStringLiteral("重采样 %1/%2").arg(row).arg(masterH));
            QApplication::processEvents(); // 保持 UI 响应
        }

        aNormRow = static_cast<double>(row) / masterH;
        rowOffRow = evalPoly(poly.aziCoeffs, 0.5, aNormRow);
        slaveRowBase = row + static_cast<int>(rowOffRow);
        syBase = rowOffRow - static_cast<int>(rowOffRow);

        int readRadius = (interpMethod == "Sinc") ? sincWindow : 2;
        int sY0 = slaveRowBase - readRadius;
        int sYH = readRadius * 2 + 1;
        if (sY0 < 0) { sYH += sY0; sY0 = 0; }
        if (sY0 + sYH > slaveH) sYH = slaveH - sY0;
        if (sYH <= 0) {
            rowBuf.fill(std::complex<float>(0, 0));
        } else {
            auto sWindow = sReader->readBandWindow(0, 0, sY0, slaveW, sYH);

            // 每列计算 col polynomial
            for (int col = 0; col < masterW; ++col) {
                double rN = static_cast<double>(col) / masterW;
                double colOff = evalPoly(poly.rangeCoeffs, rN, aNormRow);

                double sx = col + colOff;
                double sy = syBase;
                int sYi = slaveRowBase - sY0;

                if (sx >= readRadius && sx < slaveW - readRadius - 1
                    && sYi >= readRadius && sYi < sYH - readRadius - 1
                    && sWindow.size() >= slaveW * sYH) {
                    rowBuf[col] = interp2D(sWindow, slaveW, sYH, sx,
                        static_cast<double>(sYi) + syBase,
                        interpMethod, sincWindow, sincBeta);
                } else {
                    rowBuf[col] = std::complex<float>(0, 0);
                }
            }
        }

        writer->writeRow(row, rowBuf);
    }

    return true;
}

// ──────────────────────────────────────────────────────────
// [5b] 逐 burst 独立重采样 + 拼接 (TOPSAR)
// ──────────────────────────────────────────────────────────

bool RegistrationServiceImpl::resampleImagePerBurst(
    const QVector<BurstLocalPoly>& burstPolys,
    GdalSlcReader* sReader, GdalSlcWriter* writer,
    int masterW, int masterH, int slaveW, int slaveH,
    const QString& interpMethod, int sincWindow, double sincBeta,
    const QString& outputPath)
{
    if (!writer->create(outputPath, masterW, masterH, 1)) {
        qWarning() << "[Reg] 创建输出文件失败:" << writer->lastError();
        return false;
    }
    writer->setGeoTransform(0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    QVector<std::complex<float>> rowBuf(masterW);
    int burstCount = burstPolys.size();
    int step = qMax(1, masterH / 100);

    for (int b = 0; b < burstCount; ++b) {
        if (mCancelled) return false;

        const auto& bp = burstPolys[b];
        int bStart = bp.masterStartLine;
        int bEnd   = bp.masterEndLine;

        for (int row = bStart; row < bEnd; ++row) {
            if (mCancelled) return false;

            if (row % step == 0) {
                int pct = 60 + (row * 40 / masterH);
                emit progressChanged(pct,
                    QStringLiteral("重采样 burst %1/%2 行 %3")
                        .arg(b + 1).arg(burstCount).arg(row));
                QApplication::processEvents();
            }

            // 使用该 burst 的局部多项式计算偏移
            double aNormRow = static_cast<double>(row) / masterH;
            double rowOffRow = evalPoly(bp.aziCoeffs, 0.5, aNormRow);
            int slaveRowBase = row + static_cast<int>(rowOffRow);
            double syBase = rowOffRow - static_cast<int>(rowOffRow);

            int readRadius = (interpMethod == "Sinc") ? sincWindow : 2;
            int sY0 = slaveRowBase - readRadius;
            int sYH = readRadius * 2 + 1;
            if (sY0 < 0) { sYH += sY0; sY0 = 0; }
            if (sY0 + sYH > slaveH) sYH = slaveH - sY0;

            if (sYH <= 0) {
                rowBuf.fill(std::complex<float>(0, 0));
            } else {
                auto sWindow = sReader->readBandWindow(0, 0, sY0, slaveW, sYH);

                for (int col = 0; col < masterW; ++col) {
                    double rN = static_cast<double>(col) / masterW;
                    double colOff = evalPoly(bp.rangeCoeffs, rN, aNormRow);

                    double sx  = col + colOff;
                    double sy  = syBase;
                    int    sYi = slaveRowBase - sY0;

                    // 核半径感知的越界检查 (保留完整插值窗口裕量)
                    int kernelR = readRadius;
                    if (sx >= kernelR && sx < slaveW - kernelR - 1
                        && sYi >= kernelR && sYi < sYH - kernelR - 1
                        && sWindow.size() >= slaveW * sYH) {
                        rowBuf[col] = interp2D(sWindow, slaveW, sYH, sx,
                            static_cast<double>(sYi) + syBase,
                            interpMethod, sincWindow, sincBeta);
                    } else {
                        rowBuf[col] = std::complex<float>(0, 0);
                    }
                }
            }

            writer->writeRow(row, rowBuf);
        }
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

        // Catmull-Rom 插值核
        auto cubic4 = [](double t, double vm1, double v0, double v1, double v2) {
            return v0 + 0.5*t*(v1 - vm1 + t*(2.0*vm1 - 5.0*v0 + 4.0*v1 - v2
                + t*(3.0*(v0 - v1) + v2 - vm1)));
        };

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
            colValsR[c + 1] = cubic4(fy, rowValsR[0], rowValsR[1], rowValsR[2], rowValsR[3]);
            colValsI[c + 1] = cubic4(fy, rowValsI[0], rowValsI[1], rowValsI[2], rowValsI[3]);
        }
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

// ── 辅助: 从 GCP 集合拟合 2 阶多项式 ──
bool RegistrationServiceImpl::fitPolynomial(
    const QVector<CoarseGcp>& gcps,
    int masterW, int masterH,
    double* rCoeffs, double* aCoeffs)
{
    int N = gcps.size();
    if (N < 6) return false;

    double ATA[6][6] = {};
    double ATbR[6]  = {};
    double ATbA[6]  = {};

    for (const auto& g : gcps) {
        double r = static_cast<double>(g.col) / masterW;
        double a = static_cast<double>(g.row) / masterH;
        double basis[6] = {1.0, r, a, r * a, r * r, a * a};

        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 6; ++j)
                ATA[i][j] += basis[i] * basis[j];
            ATbR[i] += basis[i] * g.rangeOff;
            ATbA[i] += basis[i] * g.aziOff;
        }
    }

    return solve6x6(ATA, ATbR, rCoeffs) && solve6x6(ATA, ATbA, aCoeffs);
}
