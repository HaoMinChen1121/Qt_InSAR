#include "CoarseCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include <QMutex>

// ── profiling 文件日志 ──
static QMutex gProfileMutex;
static bool gProfileFileOpened = false;
static void profileLog(const QString& msg) {
    qDebug() << msg;
    QMutexLocker lock(&gProfileMutex);
    QString path = QCoreApplication::applicationDirPath() + "/profile_coarse.txt";
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        QTextStream ts(&f); ts << msg << "\n";
    }
}

struct CoarseWorkItem {
    int row, col;
    int slaveRow = 0;   // 经 burstPairs 匹配映射后的辅影像行
    double initRangeOff, initAziOff;
};

struct CoarseConfig {
    QString masterPath, slavePath;
    int sW, sH, winSize;
    bool useNcc;
    int searchHalf;
    bool azimuthFromOrbit = false;   // 策略: 方位偏移取轨道初值, 相关只测 range
    CorrelationMethod method = CorrelationMethod::FFT_AMPLITUDE;  // FFT 路径引擎
    SentinelDataReader* masterSdr = nullptr;
    SentinelDataReader* slaveSdr  = nullptr;
    bool useBurstCache = false;
};

struct CoarseProfile {
    qint64 readUs = 0;
    qint64 fftUs  = 0;
    qint64 peakUs = 0;
    int    count  = 0;
    void add(qint64 r, qint64 f, qint64 p) { readUs += r; fftUs += f; peakUs += p; ++count; }
};

static QVector<OffsetPoint> processCoarseBatch(
    QVector<CoarseWorkItem> batch, CoarseConfig cfg)
{
    thread_local GdalSlcReader tlMaster, tlSlave;
    thread_local QString tlMasterPath, tlSlavePath;
    thread_local CoarseProfile prof;

    int half = cfg.winSize / 2;
    QVector<OffsetPoint> results;
    results.reserve(batch.size());
    QElapsedTimer t;

    if (!cfg.useBurstCache) {
        // ── 原有路径: thread_local GdalSlcReader ──
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

        for (const auto& w : batch) {
            OffsetPoint pt;
            pt.row = w.row; pt.col = w.col;
            pt.rangeOff = w.initRangeOff;
            pt.aziOff   = w.initAziOff;

            int mX0 = w.col - half, mY0 = w.row - half;
            t.start();
            auto mWin = mR.readBandWindow(0, mX0, mY0, cfg.winSize, cfg.winSize);
            qint64 readTime = t.nsecsElapsed() / 1000;
            if (mWin.size() < cfg.winSize * cfg.winSize) continue;

            if (cfg.useNcc) {
                int slaveWinSz = cfg.winSize + 2 * cfg.searchHalf;
                int sX0 = w.col + (int)w.initRangeOff - half - cfg.searchHalf;
                int sY0 = w.slaveRow + (int)w.initAziOff - half - cfg.searchHalf;
                if (sX0 < 0 || sY0 < 0 || sX0 + slaveWinSz > cfg.sW || sY0 + slaveWinSz > cfg.sH) continue;
                t.start();
                auto sWin = sR.readBandWindow(0, sX0, sY0, slaveWinSz, slaveWinSz);
                readTime += t.nsecsElapsed() / 1000;
                if (sWin.size() < slaveWinSz * slaveWinSz) continue;
                t.start();
                int bestDx, bestDy; double subDx, subDy;
                nccCorrelate(mWin, sWin, cfg.winSize, cfg.winSize,
                             slaveWinSz, slaveWinSz, bestDx, bestDy, subDx, subDy);
                qint64 fftTime = t.nsecsElapsed() / 1000;
                if (!cfg.azimuthFromOrbit)
                    pt.rangeOff += subDx;   // TOPS: 距离偏移=几何初值 (主辅振幅 12 天去相关≈0.1,
                                            // 相关面为噪声, 任何精化都是随机游走 — 第十八轮实测
                                            // 锁到 −3px/0px vs 真值 +2.6px 的教训)
                if (!cfg.azimuthFromOrbit) pt.aziOff += subDy;
                pt.correlation = 1.0;
                prof.add(readTime, fftTime, 0);
            } else {
                int sX0c = w.col + (int)w.initRangeOff - half;
                int sY0c = w.slaveRow + (int)w.initAziOff - half;
                if (sX0c < 0 || sY0c < 0 || sX0c + cfg.winSize > cfg.sW || sY0c + cfg.winSize > cfg.sH) continue;
                t.start();
                auto sWinC = sR.readBandWindow(0, sX0c, sY0c, cfg.winSize, cfg.winSize);
                readTime += t.nsecsElapsed() / 1000;
                if (sWinC.size() < cfg.winSize * cfg.winSize) continue;

                int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
                QVector<float> surf(outRows * outCols);
                t.start();
                float maxV = correlateSurface(mWin.data(), sWinC.data(), surf.data(), cfg.winSize, cfg.winSize, cfg.method);
                qint64 fftTime = t.nsecsElapsed() / 1000;
                t.start();
                double subDx, subDy;
                findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
                qint64 peakTime = t.nsecsElapsed() / 1000;
                if (!cfg.azimuthFromOrbit)
                    pt.rangeOff += subDx;   // TOPS: 距离偏移=几何初值 (主辅振幅 12 天去相关≈0.1,
                                            // 相关面为噪声, 任何精化都是随机游走 — 第十八轮实测
                                            // 锁到 −3px/0px vs 真值 +2.6px 的教训)
                if (!cfg.azimuthFromOrbit) pt.aziOff += subDy;
                pt.correlation = maxV;
                prof.add(readTime, fftTime, peakTime);
            }
            results.append(pt);
        }
    } else {
        // ── 新路径: SentinelDataReader 缓存读取 ──
        SentinelDataReader& mSdr = *cfg.masterSdr;
        SentinelDataReader& sSdr = *cfg.slaveSdr;

        for (const auto& w : batch) {
            OffsetPoint pt;
            pt.row = w.row; pt.col = w.col;
            pt.rangeOff = w.initRangeOff;
            pt.aziOff   = w.initAziOff;

            int mX0 = w.col - half, mY0 = w.row - half;
            QVector<std::complex<float>> mWin(cfg.winSize * cfg.winSize);
            t.start();
            if (!mSdr.readWindow(mX0, mY0, cfg.winSize, cfg.winSize, mWin.data())) continue;
            qint64 readTime = t.nsecsElapsed() / 1000;

            if (cfg.useNcc) {
                int slaveWinSz = cfg.winSize + 2 * cfg.searchHalf;
                int sX0 = w.col + (int)w.initRangeOff - half - cfg.searchHalf;
                int sY0 = w.slaveRow + (int)w.initAziOff - half - cfg.searchHalf;
                if (sX0 < 0 || sY0 < 0 || sX0 + slaveWinSz > cfg.sW || sY0 + slaveWinSz > cfg.sH) continue;
                QVector<std::complex<float>> sWin(slaveWinSz * slaveWinSz);
                t.start();
                if (!sSdr.readWindow(sX0, sY0, slaveWinSz, slaveWinSz, sWin.data())) continue;
                readTime += t.nsecsElapsed() / 1000;
                t.start();
                int bestDx, bestDy; double subDx, subDy;
                nccCorrelate(mWin, sWin, cfg.winSize, cfg.winSize,
                             slaveWinSz, slaveWinSz, bestDx, bestDy, subDx, subDy);
                qint64 fftTime = t.nsecsElapsed() / 1000;
                if (!cfg.azimuthFromOrbit)
                    pt.rangeOff += subDx;   // TOPS: 距离偏移=几何初值 (主辅振幅 12 天去相关≈0.1,
                                            // 相关面为噪声, 任何精化都是随机游走 — 第十八轮实测
                                            // 锁到 −3px/0px vs 真值 +2.6px 的教训)
                if (!cfg.azimuthFromOrbit) pt.aziOff += subDy;
                pt.correlation = 1.0;
                prof.add(readTime, fftTime, 0);
            } else {
                int sX0c = w.col + (int)w.initRangeOff - half;
                int sY0c = w.slaveRow + (int)w.initAziOff - half;
                if (sX0c < 0 || sY0c < 0 || sX0c + cfg.winSize > cfg.sW || sY0c + cfg.winSize > cfg.sH) continue;
                QVector<std::complex<float>> sWinC(cfg.winSize * cfg.winSize);
                t.start();
                if (!sSdr.readWindow(sX0c, sY0c, cfg.winSize, cfg.winSize, sWinC.data())) continue;
                readTime += t.nsecsElapsed() / 1000;

                int outRows = 2 * cfg.winSize - 1, outCols = 2 * cfg.winSize - 1;
                QVector<float> surf(outRows * outCols);
                t.start();
                float maxV = correlateSurface(mWin.data(), sWinC.data(), surf.data(), cfg.winSize, cfg.winSize, cfg.method);
                qint64 fftTime = t.nsecsElapsed() / 1000;
                t.start();
                double subDx, subDy;
                findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
                qint64 peakTime = t.nsecsElapsed() / 1000;
                if (!cfg.azimuthFromOrbit)
                    pt.rangeOff += subDx;   // TOPS: 距离偏移=几何初值 (主辅振幅 12 天去相关≈0.1,
                                            // 相关面为噪声, 任何精化都是随机游走 — 第十八轮实测
                                            // 锁到 −3px/0px vs 真值 +2.6px 的教训)
                if (!cfg.azimuthFromOrbit) pt.aziOff += subDy;
                pt.correlation = maxV;
                prof.add(readTime, fftTime, peakTime);
            }
            results.append(pt);
        }
    }

    if (prof.count > 0) {
        QString msg = QStringLiteral("[Step4 PROFILE] thread %1 | points: %2 | read: %3ms | fft: %4ms | peak: %5ms | avg/point: %6ms")
            .arg((quintptr)QThread::currentThreadId() % 1000).arg(prof.count)
            .arg(prof.readUs / 1000).arg(prof.fftUs / 1000).arg(prof.peakUs / 1000)
            .arg((prof.readUs + prof.fftUs + prof.peakUs) / prof.count / 1000);
        profileLog(msg); prof = CoarseProfile{};
    }
    return results;
}

bool CoarseCorrelator::execute(PipelineContext& ctx) {
    QElapsedTimer stepTimer;
    stepTimer.start();

    const auto& p = *ctx.params;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    int nPerBurst = p.offsetPerBurst;
    int winSize = p.coarseWindowSize;
    bool useNcc = ctx.strategy
        && ctx.strategy->coarseCorr == CorrelationMethod::NCC;
    int searchHalf = useNcc ? p.coarseSearchWindow : 0;

    QVector<CoarseWorkItem> items;
    items.reserve(N * nPerBurst);
    for (int b = 0; b < N; ++b) {
        int startRow = b * L;
        for (int k = 0; k < nPerBurst; ++k) {
            CoarseWorkItem wi;
            wi.row = startRow + (k + 1) * L / (nPerBurst + 1);
            double colFrac = (nPerBurst > 1)
                ? (0.1 + 0.8 * k / (nPerBurst - 1.0)) : 0.5;
            wi.col = static_cast<int>(w * colFrac);
            wi.slaveRow = ctx.slaveRowFor(wi.row);
            for (const auto& io : ctx.initialOffsets)
                if (io.burstIndex == b) { wi.initRangeOff = io.rangeOff; wi.initAziOff = io.aziOff; break; }
            items.append(wi);
        }
    }

    int nThreads = qBound(1, QThread::idealThreadCount(), 12);
    int batchSize = qMax(1, (items.size() + nThreads * 4 - 1) / (nThreads * 4));
    QList<QVector<CoarseWorkItem>> batches;
    for (int i = 0; i < items.size(); i += batchSize) {
        QVector<CoarseWorkItem> batch;
        for (int j = i; j < qMin(i + batchSize, items.size()); ++j)
            batch.append(items[j]);
        batches.append(batch);
    }

    qDebug() << QStringLiteral("[Step4] Parallel coarse: %1 points, %2 threads, %3 batches")
        .arg(items.size()).arg(nThreads).arg(batches.size());

    CoarseConfig cfg;
    cfg.masterPath = ctx.masterLocalPath.isEmpty() ? ctx.masterBand->rasterPath : ctx.masterLocalPath;
    cfg.slavePath  = ctx.slaveLocalPath.isEmpty()  ? ctx.slaveBand->rasterPath  : ctx.slaveLocalPath;
    cfg.sW = sW; cfg.sH = sH;
    cfg.winSize = winSize;
    cfg.useNcc = useNcc;
    cfg.searchHalf = searchHalf;
    cfg.method = ctx.strategy ? ctx.strategy->coarseCorr
                              : CorrelationMethod::FFT_AMPLITUDE;
    // 策略驱动: OrbitGeometry/Hybrid → 方位取轨道初值 (TOPS 硬结论)
    cfg.azimuthFromOrbit = !ctx.strategy
        || ctx.strategy->azimuthModel == AzimuthOffsetModel::OrbitGeometry
        || ctx.strategy->azimuthModel == AzimuthOffsetModel::Hybrid;
    cfg.masterSdr = ctx.masterSdr;
    cfg.slaveSdr  = ctx.slaveSdr;
    cfg.useBurstCache = ctx.useBurstCache;

    qint64 dispatchUs = stepTimer.nsecsElapsed() / 1000;

    QElapsedTimer futureTimer;
    futureTimer.start();
    QList<QFuture<QVector<OffsetPoint>>> futures;
    for (int i = 0; i < batches.size(); ++i)
        futures.append(QtConcurrent::run(processCoarseBatch, batches[i], cfg));

    ctx.offsetPoints.clear();
    ctx.offsetPoints.reserve(items.size());
    for (auto& f : futures)
        ctx.offsetPoints.append(f.result());
    qint64 computeUs = futureTimer.nsecsElapsed() / 1000;

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
    qint64 totalUs = stepTimer.nsecsElapsed() / 1000;
    QString timingMsg = QStringLiteral("[Step4 TIMING] dispatch:%1ms compute+wait:%2ms total:%3ms")
        .arg(dispatchUs / 1000).arg(computeUs / 1000).arg(totalUs / 1000);
    profileLog(timingMsg);
    qDebug() << QStringLiteral("[Step4] %1 coarse %2/%3 valid range:[%4,%5]avg=%6 azi:[%7,%8]avg=%9")
        .arg(correlationMethodName(ctx.strategy ? ctx.strategy->coarseCorr
                                                : CorrelationMethod::FFT_AMPLITUDE))
        .arg(validN).arg(ctx.offsetPoints.size())
        .arg(minR,0,'f',2).arg(maxR,0,'f',2).arg(validN>0?sumR/validN:0,0,'f',2)
        .arg(minA,0,'f',2).arg(maxA,0,'f',2).arg(validN>0?sumA/validN:0,0,'f',2);
    return validN >= 6;
}
