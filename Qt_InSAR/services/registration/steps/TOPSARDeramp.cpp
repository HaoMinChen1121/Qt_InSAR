#include "TOPSARDeramp.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QDebug>

bool TOPSARDeramp::execute(PipelineContext& ctx)
{
    if (!ctx.isTopsar) return true;
    if (!ctx.slaveSdr) {
        ctx.errorMessage = QStringLiteral("TOPSARDeramp: slaveSdr is null");
        return false;
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
