#include "DataReader.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"

bool DataReader::execute(PipelineContext& ctx) {
    auto* mR = new GdalSlcReader();
    if (!mR->open(ctx.masterBand->rasterPath)) {
        ctx.errorMessage = QStringLiteral("DataReader: master open fail");
        delete mR; return false;
    }
    auto* sR = new GdalSlcReader();
    if (!sR->open(ctx.slaveBand->rasterPath)) {
        ctx.errorMessage = QStringLiteral("DataReader: slave open fail");
        delete mR; delete sR; return false;
    }
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
    d.slaveAzimuthFmRate = ctx.slaveBand->azimuthFmRate;
    ctx.isTopsar = (d.burstCount > 1);
    return true;
}
