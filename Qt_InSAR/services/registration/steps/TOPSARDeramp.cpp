#include "TOPSARDeramp.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QDebug>

bool TOPSARDeramp::execute(PipelineContext& ctx)
{
    if (!ctx.isTopsar) return true;
    if (!ctx.slaveSdr) {
        // GDAL 非缓存路径: 幅度相关不受 deramp 影响, 重采样阶段再对辅影像 deramp
        qDebug() << "[TOPSARDeramp] skip — no burst cache (deramp deferred to resampler)";
        return true;
    }

    double prf = ctx.data.masterAzimuthFrequency > 0
        ? ctx.data.masterAzimuthFrequency
        : ctx.masterSensorInfo.prf;
    double kt  = ctx.data.slaveAzimuthFmRate;

    if (prf <= 0 || std::abs(kt) < 1e-6) {
        // 无有效元数据则跳过 (非TOPS模式或无FM速率)
        qDebug() << "[TOPSARDeramp] skipping — prf=" << prf << "kt=" << kt;
        return true;
    }

    int burstCount = ctx.data.burstCount;
    qDebug() << "[TOPSARDeramp] deramping" << burstCount << "slave bursts prf=" << prf;
    for (int b = 0; b < burstCount; ++b) {
        ctx.slaveSdr->derampBurst(b, prf, kt);
    }
    return true;
}
