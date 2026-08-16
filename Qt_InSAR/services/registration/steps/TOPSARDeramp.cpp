#include "TOPSARDeramp.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include "algorithms/DerampCore.h"
#include <QDebug>
#include <cmath>
#include <vector>

bool TOPSARDeramp::execute(PipelineContext& ctx)
{
    if (!ctx.isTopsar) return true;
    // 仅 TOPS 偏移模型需要 deramp (SCANSAR 等其它 burst 模式不 deramp)
    if (ctx.strategy && ctx.strategy->offsetModel != OffsetModelKind::TOPS) {
        qDebug() << "[TOPSARDeramp] skip — offset model is not TOPS";
        return true;
    }
    if (!ctx.slaveSdr) {
        // GDAL 非缓存路径: 幅度相关不受 deramp 影响, 重采样阶段再对辅影像 deramp
        qDebug() << "[TOPSARDeramp] skip — no burst cache (deramp deferred to resampler)";
        return true;
    }

    double prf = ctx.data.masterAzimuthFrequency > 0
        ? ctx.data.masterAzimuthFrequency
        : ctx.masterSensorInfo.prf;

    if (prf <= 0) {
        qDebug() << "[TOPSARDeramp] skipping — prf=" << prf;
        return true;
    }

    int burstCount = ctx.data.burstCount;
    const int L = ctx.data.linesPerBurst;
    // 数据驱动实测 chirp: annotation azimuthFmRate 与数据真实 chirp 可差
    // ~750 Hz/s (2026-08-15 实测 −2195.78 vs −1450), 用错值 deramp 会在插值
    // 核内相位旋转 → 重采样输出相位=噪声且不可修复。取中间 burst 实测。
    double kt = ctx.data.slaveAzimuthFmRate;
    {
        const int mb = burstCount / 2;
        sar::ComplexSoAView v = ctx.slaveSdr->burstSoaView(mb);
        if (v.re && v.im && v.size > 0 && L > 0) {
            const int bw = static_cast<int>(v.size / L);
            if (bw > 0 && static_cast<size_t>(bw) * L <= static_cast<size_t>(v.size)) {
                double conc = 0;
                const double ktMeas = sar::measureAzimuthFmRateSoa(
                    v.re, v.im, bw, L, prf, L / 2.0, &conc);
                if (conc > 0.2) {
                    kt = ktMeas;
                    qDebug() << "[TOPSARDeramp] measured kt=" << ktMeas
                             << "(conc=" << conc << ") vs annotation"
                             << ctx.data.slaveAzimuthFmRate;
                } else {
                    qWarning() << "[TOPSARDeramp] kt measurement unreliable (conc="
                               << conc << "), fallback to annotation";
                }
            }
        }
    }

    qDebug() << "[TOPSARDeramp] deramping" << burstCount << "slave bursts prf=" << prf;
    for (int b = 0; b < burstCount; ++b) {
        ctx.slaveSdr->derampBurst(b, prf, kt);
    }

    // ── 高阶方位相位轮廓校正 (第十八轮 #2, 实验性 — 默认关闭) ──
    // ⚠ 首轮实测引入回归: 注册辅残余 chirp −8.8 → −4238 Hz/s (相位被毁),
    // 机制待诊断 (疑测量窗口与 deramp 的相互作用)。默认关, INSAR_SLAVE_PROFILE=1
    // 开启以便诊断。
    if (qEnvironmentVariableIntValue("INSAR_SLAVE_PROFILE") != 1) return true;
    for (int b = 0; b < burstCount; ++b) {
        sar::ComplexSoAView v = ctx.slaveSdr->burstSoaView(b);
        if (!v.re || !v.im || v.size <= 0) continue;
        const int bw = static_cast<int>(v.size / L);
        if (bw <= 0 || L < 66) continue;
        const int nD = L - 1;
        std::vector<double> g(nD);
        double sumMag = 0, sumRe = 0, sumIm = 0;
        const int colStep = std::max(1, bw / 64);
        for (int r = 0; r < nD; ++r) {
            double sr = 0, si = 0;
            for (int c = 0; c < bw; c += colStep) {
                const size_t k0 = static_cast<size_t>(r) * bw + c;
                const size_t k1 = k0 + bw;
                const double a1 = v.re[k1], b1 = v.im[k1], a0 = v.re[k0], b0 = v.im[k0];
                sr += a1 * a0 + b1 * b0;
                si += b1 * a0 - a1 * b0;
            }
            sumRe += sr; sumIm += si; sumMag += std::sqrt(sr * sr + si * si);
            g[r] = std::atan2(si, sr);
        }
        const double conc = sumMag > 1e-12
            ? std::sqrt(sumRe * sumRe + sumIm * sumIm) / sumMag : 0.0;
        if (conc < 0.3) continue;
        for (int r = 1; r < nD; ++r) {
            while (g[r] - g[r - 1] > M_PI) g[r] -= 2 * M_PI;
            while (g[r] - g[r - 1] < -M_PI) g[r] += 2 * M_PI;
        }
        const double xc = (nD - 1) * 0.5;
        double M[4][4] = {}, rhs[4] = {};
        for (int r = 0; r < nD; ++r) {
            const double x = r - xc;
            double pw[4] = {1.0, x, x * x, x * x * x};
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) M[i][j] += pw[i] * pw[j];
            for (int i = 0; i < 4; ++i) rhs[i] += pw[i] * g[r];
        }
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int row = col + 1; row < 4; ++row)
                if (std::abs(M[row][col]) > std::abs(M[piv][col])) piv = row;
            if (std::abs(M[piv][col]) < 1e-15) continue;
            if (piv != col)
                for (int k = 0; k < 4; ++k) std::swap(M[piv][k], M[col][k]);
            std::swap(rhs[piv], rhs[col]);
            for (int row = col + 1; row < 4; ++row) {
                const double f = M[row][col] / M[col][col];
                for (int k = col; k < 4; ++k) M[row][k] -= f * M[col][k];
                rhs[row] -= f * rhs[col];
            }
        }
        double a[4] = {0, 0, 0, 0};
        for (int row = 3; row >= 0; --row) {
            double vv = rhs[row];
            for (int k = row + 1; k < 4; ++k) vv -= M[row][k] * a[k];
            a[row] = vv / std::max(1e-15, M[row][row]);
        }
        ctx.slaveSdr->derampBurstProfile(b, a);
        if (b == 0)
            qDebug() << "[TOPSARDeramp] burst profile correction applied (conc="
                     << conc << ")";
    }
    return true;
}
