#include "OrbitInitializer.h"
#include "../PipelineContext.h"
#include "algorithms/OrbitInterpolator.h"
#include <QDebug>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {

// 按绝对 UTC 时间做 position+velocity Hermite 插值
// (relativeTime 帧各产品不对齐不能用; 线性插值在 10s 矢量间隔的弧弦差
//  ±89m, 主辅矢量网格相位差 ~0.56s → 基线残留 ±5px 锯齿 (2026-08-15
//  实测 [Step3] 初值锯齿教训); natural cubic 样条振荡 ±210m 也不用;
//  Hermite 误差 ~0.2m, 与 SNAP/GAMMA 同方案)
void hermiteAtUtc(const QList<OrbitStateVector>& orb, const QDateTime& t,
                  double& x, double& y, double& z,
                  double& vx, double& vy, double& vz)
{
    const qint64 tMs = t.toMSecsSinceEpoch();
    int i0 = 0;
    for (int i = 0; i < orb.size() - 1; ++i) {
        if (orb[i].utcTime.toMSecsSinceEpoch() <= tMs
            && orb[i + 1].utcTime.toMSecsSinceEpoch() >= tMs) { i0 = i; break; }
    }
    const auto& s0 = orb[i0];
    const auto& s1 = orb[qMin(i0 + 1, orb.size() - 1)];
    const double dt = static_cast<double>(
        s1.utcTime.toMSecsSinceEpoch() - s0.utcTime.toMSecsSinceEpoch()) / 1000.0;
    double u = (std::abs(dt) > 1e-9)
        ? (tMs - s0.utcTime.toMSecsSinceEpoch()) / 1000.0 / dt : 0.0;
    u = qMax(0.0, qMin(1.0, u));
    const double h00 = 2*u*u*u - 3*u*u + 1;
    const double h10 = u*u*u - 2*u*u + u;
    const double h01 = -2*u*u*u + 3*u*u;
    const double h11 = u*u*u - u*u;
    x = h00*s0.x + h10*dt*s0.vx + h01*s1.x + h11*dt*s1.vx;
    y = h00*s0.y + h10*dt*s0.vy + h01*s1.y + h11*dt*s1.vy;
    z = h00*s0.z + h10*dt*s0.vz + h01*s1.z + h11*dt*s1.vz;
    vx = s0.vx + u*(s1.vx - s0.vx);   // 速度仅用于视线方向, 线性混合足够
    vy = s0.vy + u*(s1.vy - s0.vy);
    vz = s0.vz + u*(s1.vz - s0.vz);
}

// ── 几何距离偏移: 主辅轨道各自在自己的 burst 方位时间插值, 求 B·û ──
// 2026-08-15 实测根因: 12 天对真实距离偏移 ~+2.8px 超出距离分辨率,
// 散斑相关≈0, 幅度相关峰值被场景周期性结构旁瓣劫持 (喀什农田 ~23m 周期
// → ±10px 旁瓣) → 相关器测出 −6.7px (真值 +2.8px)。轨道几何给出 ±1px 级
// 初值 + 相关器 ±2px 约束窗 = 唯一可靠解。
// ⚠ 两产品轨道矢量的 UTC 时间相隔 12 天: 必须各用各的时间插值
// (同一地面对 = 主 burst b 时间 / 辅 burst j 时间, 相差仅 ~0.003s);
// 用同一日期时间插值会落到矢量范围外被钳制 → 基线数千 km → 初值数千 px
// (2026-08-15 实测 IW3 初值 6901-12169 px 的教训)
// 目标视线方向 û 用球面模型 (H=693km, Re=6378137):
//   cosθ = (R²+2H·Re+H²)/(2R(H+Re));  û = sinθ·ĥ − cosθ·n̂
//   rangeOff = −B·û/rangeSpacing (dR = R_s−R_m = −B·û)
bool computeGeometricRangeOff(const QList<OrbitStateVector>& mOrb,
                              const QList<OrbitStateVector>& sOrb,
                              const QDateTime& tMasterUtc,
                              const QDateTime& tSlaveUtc,
                              double nearRange, double rangeSpacing,
                              int masterWidth,
                              double& rangeOff)
{
    rangeOff = 0.0;
    if (mOrb.size() < 2 || sOrb.size() < 2 || rangeSpacing <= 0) return false;

    double mx, my, mz, mvx, mvy, mvz;
    double sx, sy, sz, svx, svy, svz;
    hermiteAtUtc(mOrb, tMasterUtc, mx, my, mz, mvx, mvy, mvz);
    hermiteAtUtc(sOrb, tSlaveUtc, sx, sy, sz, svx, svy, svz);

    // 基线 (主辅在同一 UTC 时刻; 沿轨分量误差 ~0.1px, 忽略)
    const double bx = sx - mx, by = sy - my, bz = sz - mz;

    // 精确零多普勒视线方向 (WGS84 椭球, h=0; 与 TopoPhaseRemover 同公式):
    //   a = n̂ − (n̂·v̂)·v̂ 归一,  b = v̂×a,
    //   ψ = −acos((s²+R²−Re²)/(2·R·s))  (右视根 — 2026-08-16 数值验证:
    //     同对 12 天实测 +2.59px vs 零多普勒真值 +2.60px ✓)
    //   û = a·cosψ + b·sinψ;   rangeOff = −B·û/spacing  (dR = R_s−R_m = −B·û)
    // ⚠ 旧球面模型 (θ=acos((R²+2HRe+H²)/(2R(H+Re))), ĥ) 两处错误:
    //   ① ĥ 曾用 V×n̂ — 升轨指向西侧(镜像侧), 初值符号反号 (-2.6px vs
    //      真值 +2.6px) → 相关器在 ±2px 窗内锁到错误偏移 → 辅影像距离向
    //      错位 ~5.6px → 干涉图相位=噪声 → coh 0.25 (第十八轮根因)
    //   ② 球面 θ 近似误差 ~0.6px (同几何精确式 +1.96 vs +2.59px)
    const double R = nearRange + (masterWidth / 2.0) * rangeSpacing;
    const double rmag = std::sqrt(mx * mx + my * my + mz * mz);
    if (rmag < 1e-6) return false;
    const double nx = -mx / rmag, ny = -my / rmag, nz = -mz / rmag;   // 天底方向 (向下)
    const double vm = std::sqrt(mvx * mvx + mvy * mvy + mvz * mvz);
    if (vm < 1e-9) return false;
    const double vx = mvx / vm, vy = mvy / vm, vz = mvz / vm;
    const double ndv = nx * vx + ny * vy + nz * vz;
    double ax = nx - ndv * vx, ay = ny - ndv * vy, az = nz - ndv * vz;
    const double am = std::sqrt(ax * ax + ay * ay + az * az);
    if (am < 1e-12) return false;
    ax /= am; ay /= am; az /= am;
    const double bvx = vy * az - vz * ay;   // b = v̂ × a
    const double bvy = vz * ax - vx * az;
    const double bvz = vx * ay - vy * ax;
    constexpr double kRe = 6378137.0;
    const double sMag = rmag;
    const double cospsi = (sMag * sMag + R * R - kRe * kRe) / (2.0 * R * sMag);
    const double psi = -std::acos(qMax(-1.0, qMin(1.0, cospsi)));
    const double cp = std::cos(psi), sp = std::sin(psi);
    const double ux = ax * cp + bvx * sp;
    const double uy = ay * cp + bvy * sp;
    const double uz = az * cp + bvz * sp;

    const double bDotU = bx * ux + by * uy + bz * uz;
    rangeOff = -bDotU / rangeSpacing;
    return true;
}

} // namespace

bool OrbitInitializer::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int N = ctx.data.burstCount;
    int L = ctx.data.linesPerBurst;
    int colMid = ctx.data.masterWidth / 2;

    if (ctx.isTopsar) {
        ctx.initialOffsets.resize(N);
        const auto& mAnx = ctx.masterBand->burstAzimuthAnxTimes;
        const auto& sAnx = ctx.slaveBand->burstAzimuthAnxTimes;
        const auto& mBurstTimes = ctx.masterBand->burstAzimuthTimes;
        const auto& sBurstTimes = ctx.slaveBand->burstAzimuthTimes;
        const double prf = p.masterPrf > 0 ? p.masterPrf
            : ctx.data.masterAzimuthFrequency;
        for (int b = 0; b < N; ++b) {
            double rangeOff = 0, aziOff = 0;
            int j = b;   // 辅 burst 索引 (burstPairs 匹配结果)
            // 方位偏移 = 主辅 burst ANX 时间差 × PRF (逐 burst 常数 — 真值!)
            // 2026-08-16 第十九轮定案: ANX 差值与绝对 burst 时间差值逐 burst
            // 完全一致 (差值 = 12 天 − 0.5645s 恒定, <0.001px 散布) → ANX 值
            // 携带的逐 burst ±1px 整数变化 (实测 1.3532/0.3532/2.3532 三档)
            // 是两个雷达 burst 网格的真实相位差, 不是噪声。真正的 bug 是
            // 下游 PolynomialFitter 把逐 burst 常数拟合成平滑多项式 →
            // 重采样方位错位 ~1px (post-coreg 0.23-1.10px, TOPS 要求
            // <0.01px)。修复在 EsdCorrector: burstResults 逐 burst aziPoly
            // = [initAzi_b, 0, 0] (见该文件)。零多普勒轨道法 (δt·prf ≈
            // −274px) 只含沿轨偏移、缺 burst 起始对齐项 (+276px), 两者
            // 相消才得 +1.35px — 不可单独使用。
            if (prf > 0 && mAnx.size() > b && sAnx.size() > b) {
                if (ctx.burstPairs.size() == N
                    && ctx.burstPairs[b].isValid
                    && ctx.burstPairs[b].slaveBurstIdx >= 0
                    && ctx.burstPairs[b].slaveBurstIdx < sAnx.size())
                    j = ctx.burstPairs[b].slaveBurstIdx;
                aziOff = (mAnx[b] - sAnx[j]) * prf;
            } else {
                int centerRow = b * L + L / 2;
                computeOrbitOffset(p.masterOrbitVectors, p.slaveOrbitVectors,
                    p.masterNearRange, p.masterRangeSpacing,
                    p.masterAzimuthSpacing, p.masterPrf,
                    centerRow, colMid,
                    rangeOff, aziOff);
            }
            // 距离偏移 = 轨道几何初值 (相关器仅做 ±2px 约束窗内精化;
            // 见 computeGeometricRangeOff 注释 — 场景周期旁瓣教训)
            // 主/辅轨道各自在自己的 burst 时间插值 (同地面对)
            if (mBurstTimes.size() > b) {
                QDateTime tSlave = (sBurstTimes.size() > j)
                    ? sBurstTimes[j] : mBurstTimes[b];
                double geo = 0;
                if (computeGeometricRangeOff(p.masterOrbitVectors,
                        p.slaveOrbitVectors, mBurstTimes[b], tSlave,
                        p.masterNearRange, p.masterRangeSpacing,
                        ctx.data.masterWidth, geo)) {
                    // 合理性守卫: 同轨道对 |B| 量级 < 数百米 → |rangeOff| < ~200px;
                    // 超出说明插值时间异常 (如落出矢量范围被钳制), 回退 0
                    if (std::abs(geo) <= 200.0) {
                        rangeOff = geo;
                    } else {
                        qWarning() << "[Step3] geometric rangeOff" << geo
                                   << "px unreasonable (burst" << b << "), fallback to 0";
                    }
                }
            }
            ctx.initialOffsets[b].rangeOff = rangeOff;
            ctx.initialOffsets[b].aziOff = aziOff;
            ctx.initialOffsets[b].burstIndex = b;
        }
    } else {
        ctx.initialOffsets.resize(1);
        int centerRow = ctx.data.masterHeight / 2;
        computeOrbitOffset(p.masterOrbitVectors, p.slaveOrbitVectors,
            p.masterNearRange, p.masterRangeSpacing,
            p.masterAzimuthSpacing, p.masterPrf,
            centerRow, colMid,
            ctx.initialOffsets[0].rangeOff, ctx.initialOffsets[0].aziOff);
    }

    // 诊断: 逐 burst 初始偏移 (与 pre-fit 统计对比可定位错位来源)
    {
        QStringList inits;
        for (const auto& io : ctx.initialOffsets)
            inits << QStringLiteral("b%1:(r=%2,a=%3)")
                .arg(io.burstIndex)
                .arg(io.rangeOff, 0, 'f', 2).arg(io.aziOff, 0, 'f', 2);
        qDebug().noquote() << "[Step3] orbit init offsets:" << inits.join(QStringLiteral(" "));
    }
    return true;
}
