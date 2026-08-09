#include "FineCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>

struct FineWorkItem {
    int row, col, origIdx;
    double rangeOff, aziOff;
};

struct FineConfig {
    QString masterPath, slavePath;
    int sW, sH, winSize;
};

static QVector<OffsetPoint> processFineBatch(
    QVector<FineWorkItem> batch, FineConfig cfg)
{
    thread_local GdalSlcReader tlMaster, tlSlave;
    thread_local QString tlMasterPath, tlSlavePath;
    if (tlMasterPath != cfg.masterPath) {
        tlMaster.close();
        if (!tlMaster.open(cfg.masterPath)) return {};
        tlMasterPath = cfg.masterPath;
    }
    if (tlSlavePath != cfg.slavePath) {
        tlSlave.close();
        if (!tlSlave.open(cfg.slavePath)) return {};
        tlSlavePath = cfg.slavePath;
    }
    GdalSlcReader& mR = tlMaster;
    GdalSlcReader& sR = tlSlave;

    int half = cfg.winSize / 2;
    QVector<OffsetPoint> results;
    results.reserve(batch.size());

    for (const auto& w : batch) {
        OffsetPoint pt;
        pt.row = w.row; pt.col = w.col;
        pt.rangeOff = w.rangeOff;
        pt.aziOff   = w.aziOff;
        pt.origIdx  = w.origIdx;

        int mX0 = w.col - half, mY0 = w.row - half;
        auto mWin = mR.readBandWindow(0, mX0, mY0, cfg.winSize, cfg.winSize);
        if (mWin.size() < cfg.winSize * cfg.winSize) continue;

        int sX0 = w.col + (int)w.rangeOff - half;
        int sY0 = w.row + (int)w.aziOff - half;
        if (sX0 < 0) sX0 = 0; if (sY0 < 0) sY0 = 0;
        if (sX0 + cfg.winSize > cfg.sW) sX0 = cfg.sW - cfg.winSize;
        if (sY0 + cfg.winSize > cfg.sH) sY0 = cfg.sH - cfg.winSize;
        if (sX0 < 0 || sY0 < 0) continue;
        auto sWin = sR.readBandWindow(0, sX0, sY0, cfg.winSize, cfg.winSize);
        if (sWin.size() < cfg.winSize * cfg.winSize) continue;

        int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
        QVector<float> surf(outRows * outCols);
        float maxV = fftAmpCorrelate(mWin.data(), sWin.data(), surf.data(), cfg.winSize, cfg.winSize);

        double subDx, subDy;
        findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
        if (std::abs(subDx) > 5.0 || std::abs(subDy) > 3.0) {
            pt.correlation = -1.0;
        } else {
            pt.rangeOff += subDx;
            pt.aziOff   += subDy;
            pt.correlation = maxV;
        }
        results.append(pt);
    }
    return results;
}

bool FineCorrelator::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    if (p.route == RegRoute::Route1_OrbitFFT) return true;
    if (p.fineMethod != "FFT") return true;

    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int winSize = 128;
    int N = ctx.offsetPoints.size();
    if (N == 0) return true;

    QVector<FineWorkItem> items;
    items.reserve(N);
    for (int i = 0; i < N; ++i) {
        const auto& op = ctx.offsetPoints[i];
        items.append({op.row, op.col, i, op.rangeOff, op.aziOff});
    }

    int nThreads = qBound(1, QThread::idealThreadCount(), 12);
    int batchSize = qMax(1, (N + nThreads * 4 - 1) / (nThreads * 4));
    QList<QVector<FineWorkItem>> batches;
    for (int i = 0; i < N; i += batchSize) {
        QVector<FineWorkItem> batch;
        for (int j = i; j < qMin(i + batchSize, N); ++j)
            batch.append(items[j]);
        batches.append(batch);
    }

    qDebug() << QStringLiteral("[Step7] Parallel fine: %1 points, %2 threads, %3 batches")
        .arg(N).arg(nThreads).arg(batches.size());

    FineConfig cfg;
    cfg.masterPath = ctx.masterLocalPath.isEmpty() ? ctx.masterBand->rasterPath : ctx.masterLocalPath;
    cfg.slavePath  = ctx.slaveLocalPath.isEmpty()  ? ctx.slaveBand->rasterPath  : ctx.slaveLocalPath;
    cfg.sW = sW; cfg.sH = sH;
    cfg.winSize = winSize;

    QList<QFuture<QVector<OffsetPoint>>> futures;
    for (int i = 0; i < batches.size(); ++i)
        futures.append(QtConcurrent::run(processFineBatch, batches[i], cfg));

    int refined = 0, rejected = 0, failed = 0;
    for (auto& f : futures) {
        for (const auto& r : f.result()) {
            if (r.origIdx < 0 || r.origIdx >= N) { ++failed; continue; }
            if (r.correlation > 0) {
                ctx.offsetPoints[r.origIdx] = r; ++refined;
            } else if (r.correlation < 0) {
                ++rejected;
            } else {
                ++failed;
            }
        }
    }
    qDebug() << QStringLiteral("[Step7] fine: %1 refined, %2 rejected, %3 failed (win=%4)")
        .arg(refined).arg(rejected).arg(failed).arg(winSize);
    return true;
}
