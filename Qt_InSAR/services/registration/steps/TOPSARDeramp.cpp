#include "TOPSARDeramp.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include "algorithms/DerampCore.h"
#include <QDebug>

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
    // 数据驱动实测 chirp: annotation azimuthFmRate 与数据真实 chirp 可差
    // ~750 Hz/s (2026-08-15 实测 −2195.78 vs −1450), 用错值 deramp 会在插值
    // 核内相位旋转 → 重采样输出相位=噪声且不可修复。取中间 burst 实测。
    double kt = ctx.data.slaveAzimuthFmRate;
    {
        const int mb = burstCount / 2;
        const int L = ctx.data.linesPerBurst;
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
    return true;
}
