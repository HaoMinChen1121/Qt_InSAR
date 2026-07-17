#include "DataReader.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>

bool DataReader::execute(PipelineContext& ctx) {
    qDebug() << QStringLiteral("[DataReader] opening master: %1").arg(ctx.masterBand->rasterPath);
    auto* mR = new GdalSlcReader();
    if (!mR->open(ctx.masterBand->rasterPath)) {
        ctx.errorMessage = QStringLiteral("DataReader: master open fail");
        delete mR; return false;
    }
    qDebug() << "[DataReader] master opened ok";

    qDebug() << QStringLiteral("[DataReader] opening slave: %1").arg(ctx.slaveBand->rasterPath);
    auto* sR = new GdalSlcReader();
    if (!sR->open(ctx.slaveBand->rasterPath)) {
        ctx.errorMessage = QStringLiteral("DataReader: slave open fail");
        delete mR; delete sR; return false;
    }
    qDebug() << "[DataReader] slave opened ok";

    ctx.masterReader = mR;
    ctx.slaveReader  = sR;

    auto& d = ctx.data;
    d.masterWidth  = mR->width();
    d.masterHeight = mR->height();
    d.slaveWidth   = sR->width();
    d.slaveHeight  = sR->height();
    d.burstCount    = ctx.masterBand->burstCount;
    d.linesPerBurst = ctx.masterBand->linesPerBurst;
    d.burstStartLines    = ctx.masterBand->burstStartLines;
    d.masterBurstTimes   = ctx.masterBand->burstAzimuthTimes;
    d.slaveBurstTimes    = ctx.slaveBand->burstAzimuthTimes;
    d.slaveAzimuthFmRate       = ctx.slaveBand->azimuthFmRate;
    d.slaveAzimuthSteeringRate = ctx.slaveBand->azimuthSteeringRate;
    d.masterAzimuthFrequency   = ctx.masterBand->azimuthFrequency;
    ctx.isTopsar = (d.burstCount > 1);
    return true;
}
