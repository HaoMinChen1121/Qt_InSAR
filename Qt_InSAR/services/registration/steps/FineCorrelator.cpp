#include "FineCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>

static QMutex gFineProfileMutex;
static void fineProfileLog(const QString& msg) {
    qDebug() << msg;
    QMutexLocker lock(&gFineProfileMutex);
    QString path = QCoreApplication::applicationDirPath() + "/profile_fine.txt";
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        static bool first = true;
        QTextStream ts(&f); ts << msg << "\n";
    }
}

struct FineWorkItem {
    int row, col, origIdx;
    int slaveRow = 0;   // 经 burstPairs 匹配映射后的辅影像行
    double rangeOff, aziOff;
};

struct FineProfile {
    qint64 readUs = 0, fftUs = 0, peakUs = 0;
    int count = 0;
    void add(qint64 r, qint64 f, qint64 p) { readUs+=r; fftUs+=f; peakUs+=p; ++count; }
};

struct FineConfig {
    QString masterPath, slavePath;
    int sW, sH, winSize;
    bool azimuthFromOrbit = false;   // 策略: 方位偏移保持粗配准值, 精相关只测 range
    CorrelationMethod method = CorrelationMethod::FFT_AMPLITUDE;  // 精配准引擎
    SentinelDataReader* masterSdr = nullptr;
    SentinelDataReader* slaveSdr  = nullptr;
    bool useBurstCache = false;
};

static QVector<OffsetPoint> processFineBatch(
    QVector<FineWorkItem> batch, FineConfig cfg)
{
    thread_local GdalSlcReader tlMaster, tlSlave;
    thread_local QString tlMasterPath, tlSlavePath;
    thread_local FineProfile prof;

    int half = cfg.winSize / 2;
    QVector<OffsetPoint> results;
    results.reserve(batch.size());
    QElapsedTimer t;

    if (!cfg.useBurstCache) {
        // ── 原有路径: thread_local GdalSlcReader ──
        if (tlMasterPath != cfg.masterPath) {
            tlMaster.close(); if (!tlMaster.open(cfg.masterPath)) return {};
            tlMasterPath = cfg.masterPath;
        }
        if (tlSlavePath != cfg.slavePath) {
            tlSlave.close(); if (!tlSlave.open(cfg.slavePath)) return {};
            tlSlavePath = cfg.slavePath;
        }
        GdalSlcReader& mR = tlMaster;
        GdalSlcReader& sR = tlSlave;

        for (const auto& w : batch) {
            OffsetPoint pt; pt.row = w.row; pt.col = w.col;
            pt.rangeOff = w.rangeOff; pt.aziOff = w.aziOff; pt.origIdx = w.origIdx;

            int mX0 = w.col - half, mY0 = w.row - half;
            t.start();
            auto mWin = mR.readBandWindow(0, mX0, mY0, cfg.winSize, cfg.winSize);
            qint64 readTime = t.nsecsElapsed() / 1000;
            if (mWin.size() < cfg.winSize * cfg.winSize) continue;

            int sX0 = w.col + (int)w.rangeOff - half;
            int sY0 = w.slaveRow + (int)w.aziOff - half;
            if (sX0 < 0) sX0 = 0; if (sY0 < 0) sY0 = 0;
            if (sX0 + cfg.winSize > cfg.sW) sX0 = cfg.sW - cfg.winSize;
            if (sY0 + cfg.winSize > cfg.sH) sY0 = cfg.sH - cfg.winSize;
            if (sX0 < 0 || sY0 < 0) continue;
            t.start();
            auto sWin = sR.readBandWindow(0, sX0, sY0, cfg.winSize, cfg.winSize);
            readTime += t.nsecsElapsed() / 1000;
            if (sWin.size() < cfg.winSize * cfg.winSize) continue;

            int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
            QVector<float> surf(outRows * outCols);
            t.start();
            float maxV = correlateSurface(mWin.data(), sWin.data(), surf.data(), cfg.winSize, cfg.winSize, cfg.method);
            qint64 fftTime = t.nsecsElapsed() / 1000;

            t.start();
            double subDx, subDy;
            findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
            qint64 peakTime = t.nsecsElapsed() / 1000;

            if (cfg.azimuthFromOrbit) {
                // TOPS: 距离偏移=几何初值, 不做相关精化 (主辅振幅 12 天去相关≈0.1,
                // 相关面为噪声 — 第十八轮实测 ±2px 窗内锁到 −3px/0px vs 真值
                // +2.6px; 几何零多普勒初值精度 ~0.1px 已足够)
                pt.correlation = maxV;
            } else if (std::abs(subDx) > 5.0 || std::abs(subDy) > 3.0) {
                pt.correlation = -1.0;
            } else {
                pt.rangeOff += subDx;
                pt.aziOff += subDy;
                pt.correlation = maxV;
            }
            prof.add(readTime, fftTime, peakTime);
            results.append(pt);
        }
    } else {
        // ── 新路径: SentinelDataReader 缓存读取 ──
        SentinelDataReader& mSdr = *cfg.masterSdr;
        SentinelDataReader& sSdr = *cfg.slaveSdr;

        for (const auto& w : batch) {
            OffsetPoint pt; pt.row = w.row; pt.col = w.col;
            pt.rangeOff = w.rangeOff; pt.aziOff = w.aziOff; pt.origIdx = w.origIdx;

            int mX0 = w.col - half, mY0 = w.row - half;
            QVector<std::complex<float>> mWin(cfg.winSize * cfg.winSize);
            t.start();
            if (!mSdr.readWindow(mX0, mY0, cfg.winSize, cfg.winSize, mWin.data())) continue;
            qint64 readTime = t.nsecsElapsed() / 1000;

            int sX0 = w.col + (int)w.rangeOff - half;
            int sY0 = w.slaveRow + (int)w.aziOff - half;
            if (sX0 < 0) sX0 = 0; if (sY0 < 0) sY0 = 0;
            if (sX0 + cfg.winSize > cfg.sW) sX0 = cfg.sW - cfg.winSize;
            if (sY0 + cfg.winSize > cfg.sH) sY0 = cfg.sH - cfg.winSize;
            if (sX0 < 0 || sY0 < 0) continue;
            QVector<std::complex<float>> sWin(cfg.winSize * cfg.winSize);
            t.start();
            if (!sSdr.readWindow(sX0, sY0, cfg.winSize, cfg.winSize, sWin.data())) continue;
            readTime += t.nsecsElapsed() / 1000;

            int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
            QVector<float> surf(outRows * outCols);
            t.start();
            float maxV = correlateSurface(mWin.data(), sWin.data(), surf.data(), cfg.winSize, cfg.winSize, cfg.method);
            qint64 fftTime = t.nsecsElapsed() / 1000;

            t.start();
            double subDx, subDy;
            findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
            qint64 peakTime = t.nsecsElapsed() / 1000;

            if (cfg.azimuthFromOrbit) {
                // TOPS: 距离偏移=几何初值, 不做相关精化 (第十八轮定案, 见上)
                pt.correlation = maxV;
            } else if (std::abs(subDx) > 5.0 || std::abs(subDy) > 3.0) {
                pt.correlation = -1.0;
            } else {
                pt.rangeOff += subDx;
                pt.aziOff += subDy;
                pt.correlation = maxV;
            }
            prof.add(readTime, fftTime, peakTime);
            results.append(pt);
        }
    }

    if (prof.count > 0) {
        QString msg = QStringLiteral("[Step7 PROFILE] thread %1 | points: %2 | read: %3ms | fft: %4ms | peak: %5ms | avg/point: %6ms")
            .arg((quintptr)QThread::currentThreadId() % 1000).arg(prof.count)
            .arg(prof.readUs/1000).arg(prof.fftUs/1000).arg(prof.peakUs/1000)
            .arg((prof.readUs+prof.fftUs+prof.peakUs)/prof.count/1000);
        fineProfileLog(msg); prof = FineProfile{};
    }
    return results;
}

bool FineCorrelator::execute(PipelineContext& ctx) {
    QElapsedTimer stepTimer; stepTimer.start();
    const auto& p = *ctx.params;
    if (!ctx.strategy || !ctx.strategy->useFine) return true;

    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int winSize = p.fineWindowSize > 0 ? p.fineWindowSize : 128;
    int N = ctx.offsetPoints.size();
    if (N == 0) return true;

    QVector<FineWorkItem> items; items.reserve(N);
    for (int i = 0; i < N; ++i) {
        const auto& op = ctx.offsetPoints[i];
        items.append({op.row, op.col, i, ctx.slaveRowFor(op.row), op.rangeOff, op.aziOff});
    }

    int nThreads = qBound(1, QThread::idealThreadCount(), 12);
    int batchSize = qMax(1, (N + nThreads * 4 - 1) / (nThreads * 4));
    QList<QVector<FineWorkItem>> batches;
    for (int i = 0; i < N; i += batchSize) {
        QVector<FineWorkItem> batch;
        for (int j = i; j < qMin(i + batchSize, N); ++j) batch.append(items[j]);
        batches.append(batch);
    }

    qDebug() << QStringLiteral("[Step7] Parallel fine: %1 points, %2 threads, %3 batches").arg(N).arg(nThreads).arg(batches.size());

    FineConfig cfg;
    cfg.masterPath = ctx.masterLocalPath.isEmpty() ? ctx.masterBand->rasterPath : ctx.masterLocalPath;
    cfg.slavePath  = ctx.slaveLocalPath.isEmpty()  ? ctx.slaveBand->rasterPath  : ctx.slaveLocalPath;
    cfg.sW = sW; cfg.sH = sH; cfg.winSize = winSize;
    cfg.method = ctx.strategy ? ctx.strategy->fineCorr
                              : CorrelationMethod::FFT_AMPLITUDE;
    cfg.azimuthFromOrbit = !ctx.strategy
        || ctx.strategy->azimuthModel == AzimuthOffsetModel::OrbitGeometry
        || ctx.strategy->azimuthModel == AzimuthOffsetModel::Hybrid;
    cfg.masterSdr = ctx.masterSdr;
    cfg.slaveSdr  = ctx.slaveSdr;
    cfg.useBurstCache = ctx.useBurstCache;

    QList<QFuture<QVector<OffsetPoint>>> futures;
    for (int i = 0; i < batches.size(); ++i)
        futures.append(QtConcurrent::run(processFineBatch, batches[i], cfg));

    int refined = 0, rejected = 0, failed = 0;
    for (auto& f : futures) {
        for (const auto& r : f.result()) {
            if (r.origIdx < 0 || r.origIdx >= N) { ++failed; continue; }
            if (r.correlation > 0) { ctx.offsetPoints[r.origIdx] = r; ++refined; }
            else if (r.correlation < 0) { ++rejected; }
            else { ++failed; }
        }
    }
    qint64 totalMs = stepTimer.nsecsElapsed() / 1000000;
    fineProfileLog(QStringLiteral("[Step7 TIMING] total:%1ms").arg(totalMs));
    qDebug() << QStringLiteral("[Step7] fine: %1 refined, %2 rejected, %3 failed (win=%4)").arg(refined).arg(rejected).arg(failed).arg(winSize);
    return true;
}
