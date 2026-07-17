#include "CoarseCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include <QDebug>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include "dataaccess/impl/GdalSlcReader.h"

struct CoarseWorkItem {
    int row, col;
    double initRangeOff, initAziOff;
};

struct CoarseConfig {
    QString masterPath, slavePath;
    int sW, sH, winSize;
    bool useNcc;
    int searchHalf;
};

static QVector<OffsetPoint> processCoarseBatch(
    QVector<CoarseWorkItem> batch, CoarseConfig cfg)
{
    GdalSlcReader mR, sR;
    if (!mR.open(cfg.masterPath) || !sR.open(cfg.slavePath))
        return {};

    int half = cfg.winSize / 2;
    QVector<OffsetPoint> results;
    results.reserve(batch.size());

    for (const auto& w : batch) {
        OffsetPoint pt;
        pt.row = w.row; pt.col = w.col;
        pt.rangeOff = w.initRangeOff;
        pt.aziOff   = w.initAziOff;

        int mX0 = w.col - half, mY0 = w.row - half;
        auto mWin = mR.readBandWindow(0, mX0, mY0, cfg.winSize, cfg.winSize);
        if (mWin.size() < cfg.winSize * cfg.winSize) continue;

        if (cfg.useNcc) {
            int slaveWinSz = cfg.winSize + 2 * cfg.searchHalf;
            int sX0 = w.col + (int)w.initRangeOff - half - cfg.searchHalf;
            int sY0 = w.row + (int)w.initAziOff - half - cfg.searchHalf;
            if (sX0 < 0 || sY0 < 0 || sX0 + slaveWinSz > cfg.sW || sY0 + slaveWinSz > cfg.sH) continue;
            auto sWin = sR.readBandWindow(0, sX0, sY0, slaveWinSz, slaveWinSz);
            if (sWin.size() < slaveWinSz * slaveWinSz) continue;

            int bestDx, bestDy;
            double subDx, subDy;
            double nccVal = nccCorrelate(mWin, sWin, cfg.winSize, cfg.winSize,
                                         slaveWinSz, slaveWinSz,
                                         bestDx, bestDy, subDx, subDy);
            pt.rangeOff += subDx;
            pt.aziOff   += subDy;
            pt.correlation = nccVal;
        } else {
            int sX0c = w.col + (int)w.initRangeOff - half;
            int sY0c = w.row + (int)w.initAziOff - half;
            if (sX0c < 0 || sY0c < 0 || sX0c + cfg.winSize > cfg.sW || sY0c + cfg.winSize > cfg.sH) continue;
            auto sWinC = sR.readBandWindow(0, sX0c, sY0c, cfg.winSize, cfg.winSize);
            if (sWinC.size() < cfg.winSize * cfg.winSize) continue;

            int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
            QVector<float> surf(outRows * outCols);
            float maxV = fftAmpCorrelate(mWin.data(), sWinC.data(), surf.data(), cfg.winSize, cfg.winSize);
            double subDx, subDy;
            findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
            pt.rangeOff += subDx;
            pt.aziOff   += subDy;
            pt.correlation = maxV;
        }
        results.append(pt);
    }
    return results;
}

bool CoarseCorrelator::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    int nPerBurst = p.offsetPerBurst;
    int winSize = p.coarseWindowSize;
    bool useNcc = (p.route == RegRoute::Route2_NCC_FFTW);
    int searchHalf = useNcc ? p.coarseSearchWindow : 0;

    // 收集所有工作项
    QVector<CoarseWorkItem> items;
    items.reserve(N * nPerBurst);
    for (int b = 0; b < N; ++b) {
        int startRow = b * L;
        for (int k = 0; k < nPerBurst; ++k) {
            CoarseWorkItem wi;
            wi.row = startRow + (k + 1) * L / (nPerBurst + 1);
            wi.col = static_cast<int>(w * (0.1 + 0.8 * k / (nPerBurst - 1.0)));
            for (const auto& io : ctx.initialOffsets)
                if (io.burstIndex == b) { wi.initRangeOff = io.rangeOff; wi.initAziOff = io.aziOff; break; }
            items.append(wi);
        }
    }

    // 按线程数分批
    int nThreads = qBound(1, QThread::idealThreadCount(), 4);
    int batchSize = qMax(1, (items.size() + nThreads - 1) / nThreads);
    QList<QVector<CoarseWorkItem>> batches;
    for (int i = 0; i < items.size(); i += batchSize) {
        QVector<CoarseWorkItem> batch;
        for (int j = i; j < qMin(i + batchSize, items.size()); ++j)
            batch.append(items[j]);
        batches.append(batch);
    }

    qDebug() << QStringLiteral("[Step4] Parallel coarse: %1 points, %2 threads, %3 batches")
        .arg(items.size()).arg(nThreads).arg(batches.size());

    // 并行: 每批一个 QFuture
    CoarseConfig cfg;
    cfg.masterPath = ctx.masterBand->rasterPath;
    cfg.slavePath  = ctx.slaveBand->rasterPath;
    cfg.sW = sW; cfg.sH = sH;
    cfg.winSize = winSize;
    cfg.useNcc = useNcc;
    cfg.searchHalf = searchHalf;

    QList<QFuture<QVector<OffsetPoint>>> futures;
    for (int i = 0; i < batches.size(); ++i) {
        futures.append(QtConcurrent::run(
            processCoarseBatch, batches[i], cfg));
    }

    // 收集结果
    ctx.offsetPoints.clear();
    ctx.offsetPoints.reserve(items.size());
    for (auto& f : futures)
        ctx.offsetPoints.append(f.result());

    // 统计
    int validN = 0;
    double minR=1e9, maxR=-1e9, minA=1e9, maxA=-1e9, sumR=0, sumA=0;
    for (const auto& pt : ctx.offsetPoints) {
        if (pt.correlation >= p.correlationThreshold) {
            ++validN;
            if (pt.rangeOff < minR) minR = pt.rangeOff;
            if (pt.rangeOff > maxR) maxR = pt.rangeOff;
            if (pt.aziOff < minA) minA = pt.aziOff;
            if (pt.aziOff > maxA) maxA = pt.aziOff;
            sumR += pt.rangeOff; sumA += pt.aziOff;
        }
    }
    qDebug() << QStringLiteral("[Step4] %1 coarse %2/%3 valid range:[%4,%5]avg=%6 azi:[%7,%8]avg=%9")
        .arg(useNcc ? "NCC" : "FFT").arg(validN).arg(ctx.offsetPoints.size())
        .arg(minR,0,'f',2).arg(maxR,0,'f',2).arg(validN>0?sumR/validN:0,0,'f',2)
        .arg(minA,0,'f',2).arg(maxA,0,'f',2).arg(validN>0?sumA/validN:0,0,'f',2);
    return validN >= 6;
}
