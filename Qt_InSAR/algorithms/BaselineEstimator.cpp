#include "BaselineEstimator.h"
#include <QtMath>
#include <QDebug>
#include <algorithm>

namespace sar {

static void interpolateOrbit(const QList<OrbitStateVector>& orbit,
    double targetTime, double& x, double& y, double& z,
    double& vx, double& vy, double& vz)
{
    if (orbit.size() < 2) {
        if (!orbit.isEmpty()) {
            x = orbit.first().x; y = orbit.first().y; z = orbit.first().z;
            vx = orbit.first().vx; vy = orbit.first().vy; vz = orbit.first().vz;
        }
        return;
    }

    // 找到包含 targetTime 的区间
    int i0 = 0;
    for (int i = 0; i < orbit.size() - 1; ++i) {
        if (orbit[i].relativeTime <= targetTime && orbit[i + 1].relativeTime >= targetTime) {
            i0 = i; break;
        }
    }
    const auto& sv0 = orbit[i0];
    const auto& sv1 = orbit[std::min(i0 + 1, orbit.size() - 1)];

    double dt = sv1.relativeTime - sv0.relativeTime;
    double t = (std::abs(dt) > 1e-9) ? (targetTime - sv0.relativeTime) / dt : 0.0;
    t = std::max(0.0, std::min(1.0, t));

    x = sv0.x + t * (sv1.x - sv0.x);
    y = sv0.y + t * (sv1.y - sv0.y);
    z = sv0.z + t * (sv1.z - sv0.z);
    vx = sv0.vx + t * (sv1.vx - sv0.vx);
    vy = sv0.vy + t * (sv1.vy - sv0.vy);
    vz = sv0.vz + t * (sv1.vz - sv0.vz);
}

BaselineResult computeBaseline(
    const QList<OrbitStateVector>& masterOrbit,
    const QList<OrbitStateVector>& slaveOrbit,
    const QDateTime& masterTime,
    const QDateTime& slaveTime,
    double nearRange,
    double farRange)
{
    BaselineResult result;

    if (masterOrbit.size() < 2 || slaveOrbit.size() < 2)
        return result;

    // 时间基线 (天) — 绝对采集时间差
    if (masterTime.isValid() && slaveTime.isValid())
        result.temporalBaseline = std::abs(masterTime.msecsTo(slaveTime)) / 86400000.0;

    // 场景中点: 两轨道各自插值到"自身轨道跨度中点" (同一沿轨相位)
    // 注意不能用同一绝对 UTC 时刻 — 两景相隔 12 天, 辅轨道跨度根本不覆盖
    // 主景中点时刻; 主辅同相位采样 (各 +span/2) 才是干涉基线定义
    // (2026-08-15 实测: 同相位 |B|≈3310m 稳定, 与 ASF 归一化基线
    //  -7.0947 mrad × R·sinθ 交叉验证一致)
    const double mT0 = masterOrbit.first().relativeTime;
    const double mT1 = masterOrbit.last().relativeTime;
    const double mMid = (mT0 + mT1) * 0.5;
    const double sMid = (slaveOrbit.first().relativeTime + slaveOrbit.last().relativeTime) * 0.5;

    // 插值主辅位置到各自场景中点 (同一沿轨相位)
    double mx, my, mz, mvx, mvy, mvz;
    double sx, sy, sz, svx, svy, svz;
    interpolateOrbit(masterOrbit, mMid, mx, my, mz, mvx, mvy, mvz);
    interpolateOrbit(slaveOrbit, sMid, sx, sy, sz, svx, svy, svz);

    // 基线向量 (master → slave)
    double bx = sx - mx;
    double by = sy - my;
    double bz = sz - mz;
    double bLen = std::sqrt(bx*bx + by*by + bz*bz);
    if (bLen < 1e-6) return result;

    // 估算 LOS 单位向量: 从卫星到地面点的方向
    // 简化: 使用场景中心 mid-range 的 LOS
    // LOS ≈ 从卫星到地面目标的方向 (单位向量)
    // 使用 mid-scene slant range 和地球半径估算
    const double Re = 6371000.0;  // 地球平均半径 (m)
    double midRange = (nearRange + farRange) * 0.5;
    if (midRange <= 0) midRange = 830000.0; // S1 IW 中值近似

    // 卫星到地心的距离 ≈ sqrt(Rx²+Ry²+Rz²)
    double Rsat = std::sqrt(mx*mx + my*my + mz*mz);
    // 按余弦定理求入射角: cos(inc) = (Rsat²+R²-Re²)/(2*Rsat*R)
    double cosInc = (Rsat*Rsat + midRange*midRange - Re*Re) / (2.0 * Rsat * midRange);
    cosInc = std::max(-1.0, std::min(1.0, cosInc));
    double sinInc = std::sqrt(1.0 - cosInc*cosInc);

    // LOS 方向: 从卫星指向地面 = 下视方向 (径向向内)
    // 简化: 把卫星位置方向作为径向分量
    double losX = -mx / Rsat;  // 径向指向地心
    double losY = -my / Rsat;
    double losZ = -mz / Rsat;

    // 平行基线 = 基线在LOS方向上的投影
    double bDotLos = bx * losX + by * losY + bz * losZ;
    result.parBaseline = bDotLos;

    // 垂直基线 = sqrt(总基线² - 平行基线²)
    double perp2 = bLen * bLen - bDotLos * bDotLos;
    result.perpBaseline = perp2 > 0 ? std::sqrt(perp2) : 0.0;

    result.valid = true;

    qDebug() << QStringLiteral("[Baseline] B=%1m Bperp=%2m Bpar=%3m dt=%4days")
        .arg(bLen, 0, 'f', 1)
        .arg(result.perpBaseline, 0, 'f', 1)
        .arg(result.parBaseline, 0, 'f', 1)
        .arg(result.temporalBaseline, 0, 'f', 1);

    return result;
}

} // namespace sar
