#include "TopoPhaseRemover.h"
#include "../PipelineContext.h"
#include "GeomTable.h"
#include "TopsarDeburst.h"
#include "algorithms/OrbitInterpolator.h"
#include "dataaccess/impl/GdalDemReader.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>
#include <ogr_spatialref.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr double kEarthRadius = 6378137.0;      // WGS84 半长轴
constexpr double kEccentricity2 = 0.00669437999014;

struct Vec3 { double x = 0, y = 0, z = 0; };

double vNorm(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
double vDot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// 场景地理覆盖 → DEM 像元区域读取 + 双线性采样 (lat/lon 弧度输入)
class DemRegion
{
public:
    bool load(GdalDemReader& dem, double latMin, double latMax,
              double lonMin, double lonMax, int margin)
    {
        mTransform = nullptr;
        OGRSpatialReference src;
        src.SetWellKnownGeogCS("WGS84");
        OGRSpatialReference dst;
        dst.importFromWkt(dem.projection().toUtf8().constData());
        // GDAL3 轴序: 显式传统序 (lon,lat)->(E,N) — 实测默认 lat-first
        // 会交换输入 (足迹正确仍被误判不覆盖, 2026-08-16 定位)
        src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        mTransform = OGRCreateCoordinateTransformation(&src, &dst);
        if (!mTransform) return false;

        const double* gt = dem.geoTransform();
        if (!gt || gt[1] == 0.0 || gt[5] == 0.0) return false;
        for (int i = 0; i < 6; ++i) mGt[i] = gt[i];

        // 角点 → DEM 像元, 取 bbox
        double px[4], py[4];
        const double lats[4] = {latMin, latMin, latMax, latMax};
        const double lons[4] = {lonMin, lonMax, lonMin, lonMax};
        bool anyValid = false;
        for (int i = 0; i < 4; ++i) {
            toPixel(lats[i], lons[i], px[i], py[i]);
            if (px[i] >= 0 && py[i] >= 0
                && px[i] < dem.width() && py[i] < dem.height())
                anyValid = true;
        }
        if (!anyValid) return false;

        double pmin = std::min(std::min(px[0], px[1]), std::min(px[2], px[3]));
        double pmax = std::max(std::max(px[0], px[1]), std::max(px[2], px[3]));
        double qmin = std::min(std::min(py[0], py[1]), std::min(py[2], py[3]));
        double qmax = std::max(std::max(py[0], py[1]), std::max(py[2], py[3]));
        mCol0 = std::max(0, static_cast<int>(pmin) - margin);
        mRow0 = std::max(0, static_cast<int>(qmin) - margin);
        int col1 = std::min(dem.width() - 1, static_cast<int>(pmax) + margin);
        int row1 = std::min(dem.height() - 1, static_cast<int>(qmax) + margin);
        mW = col1 - mCol0 + 1;
        mH = row1 - mRow0 + 1;
        if (mW < 2 || mH < 2) return false;

        mData = dem.readElevationWindow(mCol0, mRow0, mW, mH);
        if (mData.size() != mW * mH) return false;
        const double nd = dem.noDataValue();
        for (float& v : mData)
            if (v <= static_cast<float>(nd)) v = 0.0f;
        return true;
    }

    double sample(double latRad, double lonRad) const
    {
        if (!mTransform || mData.isEmpty()) return 0.0;
        double px, py;
        toPixel(latRad, lonRad, px, py);
        px -= mCol0;
        py -= mRow0;
        if (px < 0 || py < 0 || px >= mW - 1 || py >= mH - 1) return 0.0;
        const int i0 = static_cast<int>(px);
        const int j0 = static_cast<int>(py);
        const double fx = px - i0, fy = py - j0;
        const double v00 = mData[j0 * mW + i0];
        const double v01 = mData[j0 * mW + i0 + 1];
        const double v10 = mData[(j0 + 1) * mW + i0];
        const double v11 = mData[(j0 + 1) * mW + i0 + 1];
        return (v00 * (1 - fx) + v01 * fx) * (1 - fy)
             + (v10 * (1 - fx) + v11 * fx) * fy;
    }

private:
    void toPixel(double latRad, double lonRad, double& px, double& py) const
    {
        double x = lonRad * 180.0 / M_PI;
        double y = latRad * 180.0 / M_PI;
        mTransform->Transform(1, &x, &y);
        const double denom = mGt[1] * mGt[5] - mGt[2] * mGt[4];
        px = (mGt[5] * (x - mGt[0]) - mGt[2] * (y - mGt[3])) / denom;
        py = (-mGt[4] * (x - mGt[0]) + mGt[1] * (y - mGt[3])) / denom;
    }

    OGRCoordinateTransformation* mTransform = nullptr;
    double mGt[6] = {};
    int mCol0 = 0, mRow0 = 0, mW = 0, mH = 0;
    QVector<float> mData;
};

// 零多普勒 + WGS84 椭球 + DEM 高程迭代定位 (S/V: ECEF 位置/速度; R: 斜距)
void geolocate(const Vec3& S, const Vec3& V, double R, const DemRegion& dem,
               double& latRad, double& lonRad)
{
    const double sNorm = vNorm(S);
    const double vNorm0 = vNorm(V);
    Vec3 n = {-S.x / sNorm, -S.y / sNorm, -S.z / sNorm};
    Vec3 vh = {V.x / vNorm0, V.y / vNorm0, V.z / vNorm0};
    const double ndv = vDot(n, vh);
    Vec3 a = {n.x - ndv * vh.x, n.y - ndv * vh.y, n.z - ndv * vh.z};
    const double aNorm = vNorm(a);
    a = {a.x / aNorm, a.y / aNorm, a.z / aNorm};
    const Vec3 b = {vh.y * a.z - vh.z * a.y,
                    vh.z * a.x - vh.x * a.z,
                    vh.x * a.y - vh.y * a.x};

    // 近视侧初始角 (视线与天底的夹角). 旧公式符号取反 → 从反天底侧起步,
    // 牛顿第一步超调 ~136° → 振荡 → 远距列落到镜像交点 —
    // 实测足迹横跨整个轨道弧段 (lat 36.4..41.3 / lon 61.3..80.9) 的根因.
    // ψ 取负号 = 右侧视: u = a·cosψ + b·sinψ, b = v̂×a 是左手侧,
    // 右侧视 S1 的 u 需要负的 b 分量 (升轨右视实测验证, Python 复刻
    // 修正后足迹 lat 39.2..41.0 / lon 75.1..78.3 与场景吻合)
    double cospsi = (sNorm * sNorm + R * R - kEarthRadius * kEarthRadius)
        / (2.0 * R * sNorm);
    cospsi = std::max(-1.0, std::min(1.0, cospsi));
    double psi = -std::acos(cospsi);

    for (int it = 0; it < 8; ++it) {
        const double cp = std::cos(psi), sp = std::sin(psi);
        const Vec3 u = {a.x * cp + b.x * sp, a.y * cp + b.y * sp, a.z * cp + b.z * sp};
        const Vec3 P = {S.x + R * u.x, S.y + R * u.y, S.z + R * u.z};
        const double lat = std::atan2(P.z, std::sqrt(P.x * P.x + P.y * P.y));
        const double lon = std::atan2(P.y, P.x);
        latRad = lat;
        lonRad = lon;
        const double h = dem.sample(lat, lon);
        const double sinLat = std::sin(lat);
        const double N = kEarthRadius / std::sqrt(1.0 - kEccentricity2 * sinLat * sinLat);
        const double rho2 = P.x * P.x + P.y * P.y + P.z * P.z / (1.0 - kEccentricity2);
        const double target = (N + h) * (N + h);
        const double f = rho2 - target;
        const double dpsi = 1e-6;
        const double cp2 = std::cos(psi + dpsi), sp2 = std::sin(psi + dpsi);
        const Vec3 u2 = {a.x * cp2 + b.x * sp2, a.y * cp2 + b.y * sp2, a.z * cp2 + b.z * sp2};
        const Vec3 P2 = {S.x + R * u2.x, S.y + R * u2.y, S.z + R * u2.z};
        const double rho2b = P2.x * P2.x + P2.y * P2.y + P2.z * P2.z / (1.0 - kEccentricity2);
        const double df = (rho2b - rho2) / dpsi;
        if (std::abs(df) < 1e-12) break;
        psi -= f / df;
    }
}

// deburst 输出行 → 方位时间 (秒, 相对 mergeTimeRef) 的分段映射
struct RowTimeMap
{
    QVector<double> burstT0;      // 每 burst 首行时间 (s)
    QVector<int>    outStart;     // 每 burst 在 deburst 输出的起始行
    QVector<int>    discardTop;
    int azLooks = 1;
    double prf = 1.0;

    bool build(const QsarBand& band, int looks, const QDateTime& tRef)
    {
        const int N = band.burstCount;
        if (N < 1 || band.linesPerBurst < 1 || band.azimuthFrequency <= 0) return false;
        prf = band.azimuthFrequency;
        azLooks = std::max(1, looks);
        QVector<int> discardBottom;
        computeBurstDiscard(N, band.linesPerBurst, prf,
                            band.burstAzimuthTimes, discardTop, discardBottom);
        outStart.fill(0, N);
        int cum = 0;
        for (int b = 0; b < N; ++b) {
            outStart[b] = cum;
            const int valid = band.linesPerBurst - discardTop[b] - discardBottom[b];
            cum += std::max(0, valid) / azLooks;
        }
        burstT0.resize(N);
        for (int b = 0; b < N; ++b) {
            burstT0[b] = (b < band.burstAzimuthTimes.size() && band.burstAzimuthTimes[b].isValid())
                ? tRef.msecsTo(band.burstAzimuthTimes[b]) / 1000.0
                : (b * band.linesPerBurst) / prf;   // 无时间回退: 均匀 burst 间隔
        }
        return true;
    }

    // deburst 输出行 r 的方位时间 (s); 行中心 + 半多视窗
    double timeOfDeburstRow(int r) const
    {
        int b = 0;
        for (int i = 0; i < outStart.size() - 1; ++i)
            if (r >= outStart[i + 1]) b = i + 1;
        const int k = r - outStart[b];
        return burstT0[b] + (discardTop[b] + k * azLooks + azLooks * 0.5) / prf;
    }
};

} // namespace

// Step 3: 差分干涉 — DEM 地形相位去除
//   φ_topo = −4π/λ · B⊥ · h / (R·sinθ)
//   高程 h 由零多普勒定位 (轨道矢量 + burst 方位时间 + WGS84 椭球
//   + DEM 高程迭代) 逐点求取 — 替代旧版 LinearSlantRangeMapper 线性近似
bool TopoPhaseRemover::execute(IfgPipelineContext& ctx)
{
    const QString flatSrc = ctx.flatSourcePath.isEmpty()
        ? ctx.mergeOutputBase + "_ifg.tif" : ctx.flatSourcePath;
    const QString demPath = ctx.params->demPath;

    // 失败时把原因写入输出目录 (不依赖调试器日志; 进度栏同步显示)
    auto fail = [&](const QString& msg) -> bool {
        ctx.errorMessage = msg;
        QFile ef(ctx.diffOutputBase + "_error.txt");
        if (ef.open(QIODevice::WriteOnly)) {
            QString detail = msg + QStringLiteral("\n"
                "  demPath=%1\n"
                "  masterOrbit vectors=%2\n"
                "  mergeTimeRef valid=%3  masterPrf=%4  masterBand=%5\n")
                .arg(demPath)
                .arg(ctx.masterOrbit.size())
                .arg(ctx.mergeTimeRef.isValid() ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(ctx.masterPrf)
                .arg(ctx.masterBandInfo ? QStringLiteral("set") : QStringLiteral("null"));
            if (ctx.masterBandInfo)
                detail += QStringLiteral("  band burstCount=%1 linesPerBurst=%2 burstTimes=%3\n")
                    .arg(ctx.masterBandInfo->burstCount)
                    .arg(ctx.masterBandInfo->linesPerBurst)
                    .arg(ctx.masterBandInfo->burstAzimuthTimes.size());
            ef.write(detail.toUtf8());
            ef.close();
        }
        return false;
    };

    GeomTable geom;
    if (!geom.load(ctx.geomTablePath))
        return fail(QStringLiteral("TopoPhaseRemover: cannot load geom table %1")
                        .arg(ctx.geomTablePath));
    GdalSlcReader reader;
    if (!reader.open(flatSrc))
        return fail(QStringLiteral("TopoPhaseRemover: cannot open input %1").arg(flatSrc));
    const int w = reader.width(), h = reader.height();

    if (ctx.masterOrbit.size() < 2) {
        reader.close();
        return fail(QStringLiteral(
            "TopoPhaseRemover: master orbit state vectors unavailable "
            "(master must be an original SLC product, not .qsar)"));
    }
    if (!ctx.mergeTimeRef.isValid() || ctx.masterPrf <= 0 || !ctx.masterBandInfo) {
        reader.close();
        return fail(QStringLiteral(
            "TopoPhaseRemover: merge azimuth timing unavailable (mergeTimeRef/prf/band)"));
    }

    // 行→方位时间映射 (deburst 分段 + merge 裁剪)
    RowTimeMap rtm;
    if (!rtm.build(*ctx.masterBandInfo, ctx.params->azimuthLooks, ctx.mergeTimeRef)) {
        reader.close();
        return fail(QStringLiteral("TopoPhaseRemover: cannot build row-time map"));
    }

    GdalDemReader dem;
    if (!dem.open(demPath)) {
        reader.close();
        return fail(QStringLiteral("TopoPhaseRemover: cannot open DEM %1").arg(demPath));
    }

    const SarSensorInfo& si = ctx.masterSensorInfo;
    const double wavelength = si.wavelength > 0 ? si.wavelength : ctx.params->wavelength;
    const double Bperp = ctx.params->baselinePerp;

    // ── 零多普勒定位 (抽稀网格 16 列 × 8 行; 第一遍 h=0 求覆盖) ──
    const int stepC = 16, stepR = 8;
    const int nC = (w + stepC - 1) / stepC;
    const int nR = (h + stepR - 1) / stepR;
    QVector<double> gridLat(nC * nR), gridLon(nC * nR);
    QVector<double> gridSx(nC * nR), gridSy(nC * nR), gridSz(nC * nR);
    QVector<double> gridVx(nC * nR), gridVy(nC * nR), gridVz(nC * nR);
    QVector<double> gridR(nC * nR);

    const double orbitT0 = ctx.masterOrbit.first().relativeTime;
    const QDateTime orbitEpoch = ctx.masterOrbit.first().utcTime;

    auto orbitAt = [&](double tAbsSecFromTRef, double& x, double& y, double& z,
                       double& vx, double& vy, double& vz) {
        const QDateTime tAbs = ctx.mergeTimeRef.addMSecs(
            static_cast<qint64>(tAbsSecFromTRef * 1000.0));
        const double tRel = orbitT0 + orbitEpoch.msecsTo(tAbs) / 1000.0;
        interpolateOrbit(ctx.masterOrbit, tRel, x, y, z, vx, vy, vz);
    };

    DemRegion region;
    double latMin = 1e9, latMax = -1e9, lonMin = 1e9, lonMax = -1e9;
    for (int ir = 0; ir < nR; ++ir) {
        const int row = std::min(ir * stepR, h - 1);
        const double t = rtm.timeOfDeburstRow(ctx.mergeRow0Offset + row);
        double sx, sy, sz, vx, vy, vz;
        orbitAt(t, sx, sy, sz, vx, vy, vz);
        for (int ic = 0; ic < nC; ++ic) {
            const int col = std::min(ic * stepC, w - 1);
            double R = 0, th = 0;
            if (!geom.colGeometry(col, &R, &th)) R = 0;
            const int idx = ir * nC + ic;
            gridSx[idx] = sx; gridSy[idx] = sy; gridSz[idx] = sz;
            gridVx[idx] = vx; gridVy[idx] = vy; gridVz[idx] = vz;
            gridR[idx] = R;
            if (R > 0) {
                double lat = 0, lon = 0;
                geolocate({sx, sy, sz}, {vx, vy, vz}, R, region, lat, lon);
                gridLat[idx] = lat; gridLon[idx] = lon;
                latMin = std::min(latMin, lat); latMax = std::max(latMax, lat);
                lonMin = std::min(lonMin, lon); lonMax = std::max(lonMax, lon);
            } else {
                gridLat[idx] = gridLon[idx] = 0;
            }
        }
    }
    qDebug() << "[TopoPhase] footprint lat"
             << QString::number(latMin * 180 / M_PI, 'f', 3) << ".."
             << QString::number(latMax * 180 / M_PI, 'f', 3) << "lon"
             << QString::number(lonMin * 180 / M_PI, 'f', 3) << ".."
             << QString::number(lonMax * 180 / M_PI, 'f', 3);

    // ── 调试转储 (INSAR_TOPO_DEBUG=1): 第一遍定位网格的稀疏采样点,
    // 供 Python 逐点比对定位链路 (第十八轮 φ_topo 映射排查) ──
    if (qEnvironmentVariableIntValue("INSAR_TOPO_DEBUG") == 1) {
        QFile dbg(ctx.diffOutputBase + "_topodebug.txt");
        if (dbg.open(QIODevice::WriteOnly)) {
            QTextStream ts(&dbg);
            ts << "# ir ic row col tSec sx sy sz latDeg lonDeg R\n";
            for (int ir = 0; ir < nR; ir += qMax(1, nR / 20)) {
                for (int ic = 0; ic < nC; ic += qMax(1, nC / 20)) {
                    const int idx = ir * nC + ic;
                    const int row = std::min(ir * stepR, h - 1);
                    const int col = std::min(ic * stepC, w - 1);
                    const double t = rtm.timeOfDeburstRow(ctx.mergeRow0Offset + row);
                    ts << ir << " " << ic << " " << row << " " << col << " "
                       << QString::number(t, 'f', 6) << " "
                       << QString::number(gridSx[idx], 'f', 3) << " "
                       << QString::number(gridSy[idx], 'f', 3) << " "
                       << QString::number(gridSz[idx], 'f', 3) << " "
                       << QString::number(gridLat[idx] * 180 / M_PI, 'f', 6) << " "
                       << QString::number(gridLon[idx] * 180 / M_PI, 'f', 6) << " "
                       << QString::number(gridR[idx], 'f', 3) << "\n";
                }
            }
            ts << "# mergeRow0Offset=" << ctx.mergeRow0Offset << "\n";
            dbg.close();
            qDebug() << "[TopoPhase] debug dump written:" << ctx.diffOutputBase + "_topodebug.txt";
        }
    }

    // ── DEM 覆盖校验 + 区域读取 ──
    // margin: 第二遍带高程重迭代的点位相对 h=0 点位移动 ~h/tan(θ) ≈ 2-3km
    // (h≈1300-4800m, θ≈32-44°) — 旧值 2 像素(160m) 使边缘点出界采样得 0
    // → 地形去除在足迹边缘失效 (第十八轮 φ_topo 逐点比对定位)
    if (!region.load(dem, latMin, latMax, lonMin, lonMax, 48)) {
        reader.close();
        dem.close();
        return fail(QStringLiteral(
            "TopoPhaseRemover: DEM %1 does not cover the scene footprint "
            "(lat %2..%3, lon %4..%5)").arg(demPath)
            .arg(latMin * 180 / M_PI, 0, 'f', 2).arg(latMax * 180 / M_PI, 0, 'f', 2)
            .arg(lonMin * 180 / M_PI, 0, 'f', 2).arg(lonMax * 180 / M_PI, 0, 'f', 2));
    }

    // ── 第二遍: 带 DEM 高程重迭代 + 双线性扩展到全网格 ──
    QVector<double> hGrid(nC * nR);
    double hMin = 1e9, hMax = -1e9;
    for (int i = 0; i < nC * nR; ++i) {
        if (gridR[i] <= 0) { hGrid[i] = 0; continue; }
        double lat = gridLat[i], lon = gridLon[i];
        geolocate({gridSx[i], gridSy[i], gridSz[i]},
                  {gridVx[i], gridVy[i], gridVz[i]}, gridR[i], region, lat, lon);
        hGrid[i] = region.sample(lat, lon);
        hMin = std::min(hMin, hGrid[i]);
        hMax = std::max(hMax, hGrid[i]);
    }
    qDebug() << "[TopoPhase] DEM elevation range" << hMin << ".." << hMax << "m";

    QVector<double> hFull(w * h);
    {
        for (int r = 0; r < h; ++r) {
            double fr = (r - 0.0) / stepR;
            int ir = static_cast<int>(fr);
            fr -= ir;
            ir = std::min(ir, nR - 2);
            for (int c = 0; c < w; ++c) {
                double fc = c / static_cast<double>(stepC);
                int ic = static_cast<int>(fc);
                fc -= ic;
                ic = std::min(ic, nC - 2);
                const double v00 = hGrid[ir * nC + ic];
                const double v01 = hGrid[ir * nC + ic + 1];
                const double v10 = hGrid[(ir + 1) * nC + ic];
                const double v11 = hGrid[(ir + 1) * nC + ic + 1];
                hFull[r * w + c] = (v00 * (1 - fc) + v01 * fc) * (1 - fr)
                                 + (v10 * (1 - fc) + v11 * fc) * fr;
            }
        }
    }

    // 逐列系数 k(c) = −4π/λ · B⊥ / (R·sinθ)
    QVector<double> kc(w);
    for (int c = 0; c < w; ++c) {
        double R = 0, th = 0;
        kc[c] = (geom.colGeometry(c, &R, &th) && R > 0 && std::abs(std::sin(th)) > 1e-9)
            ? -4.0 * M_PI / std::max(wavelength, 1e-9) * Bperp / (R * std::sin(th))
            : 0.0;
    }

    // ── 旋转输出 ──
    const QString outBase = ctx.diffOutputBase;
    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const QString diffPath = outBase + "_diff.tif";
    const QString phasePath = outBase + "_diff_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, diffPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) {
        ctx.errorMessage = "TopoPhaseRemover: cannot create output";
        reader.close(); dem.close(); return false;
    }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);
    for (int row = 0; row < h; ++row) {
        if (ctx.cancelled) { GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close(); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            const double phiTopo = kc[col] * hFull[row * w + col];
            const float c = std::cos(static_cast<float>(phiTopo));
            const float s = std::sin(static_cast<float>(phiTopo));
            const auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            const auto diffVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = diffVal;
            rowPhase[col] = std::atan2(diffVal.imag(), diffVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1, reinterpret_cast<CFloat32*>(rowBuf.data()), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1, rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh);
    reader.close();
    dem.close();

    ctx.outputBand.diffFile = QStringLiteral("diff/S1_%1_diff.tif")
        .arg(ctx.outputBand.polarization);
    ctx.outputBand.diffPhaseFile = QStringLiteral("diff/S1_%1_diff_phase.tif")
        .arg(ctx.outputBand.polarization);
    qDebug() << "[TopoPhase] SUCCESS (zero-Doppler geolocation)" << w << "x" << h
             << "->" << diffPath;
    return true;
}
