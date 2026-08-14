#include "SincResampler.h"
#include "../PipelineContext.h"
#include "algorithms/SincInterpolator.h"
#include "algorithms/SincInterpCore.h"
#include "algorithms/ComplexSoA.h"
#include "algorithms/DerampCore.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalSlcWriter.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QApplication>
#include <QThread>
#include <QtConcurrent>
#include <QFuture>
#include <gdal_priv.h>
#include <algorithm>
#include <cmath>

static QMutex gSincProfileMutex;
static void sincProfileLog(const QString& msg) {
    qDebug() << msg;
    QMutexLocker lock(&gSincProfileMutex);
    QString path = QCoreApplication::applicationDirPath() + "/profile_sinc.txt";
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        QTextStream ts(&f); ts << msg << "\n";
    }
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool SincResampler::resampleNonTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    if (ctx.masterReader)
        writer.copyGeoreferencing(ctx.masterReader->datasetHandle(), QString());
    qDebug() << QStringLiteral("[Step9] non-TOPSAR resample %1x%2 method=%3").arg(mW).arg(mH).arg(p.resamplingMethod);

    QVector<std::complex<float>> rowBuf(mW);
    int step = std::max(1, mH / 100);
    for (int row = 0; row < mH; ++row) {
        if (mCancelled) return false;
        double aLoc = static_cast<double>(row) / mH;

        // 逐列方位偏移 (range-dependent azimuth): 条带覆盖 min/max
        double minRo = 1e9, maxRo = -1e9;
        QVector<double> rowOffs(mW);
        for (int c = 0; c < mW; ++c) {
            double rN = static_cast<double>(c) / mW;
            rowOffs[c] = ctx.aziPoly.coeffs[0] + ctx.aziPoly.coeffs[1] * aLoc
                       + ctx.aziPoly.coeffs[2] * rN;
            minRo = qMin(minRo, rowOffs[c]);
            maxRo = qMax(maxRo, rowOffs[c]);
        }
        int minRoI = (int)std::floor(minRo), maxRoI = (int)std::ceil(maxRo);

        int sY0 = row + minRoI - readR;
        int sYH = (maxRoI - minRoI) + readR * 2 + 1;
        if (sY0 < 0) { sYH += sY0; sY0 = 0; }
        if (sY0 + sYH > sH) sYH = sH - sY0;
        if (sYH <= 0) { rowBuf.fill({0, 0}); }
        else {
            auto sWin = ctx.slaveReader->readBandWindow(0, 0, sY0, sW, sYH);
            for (int c = 0; c < mW; ++c) {
                double rN = static_cast<double>(c) / mW;
                double colOff = ctx.rangePoly.coeffs[0] + ctx.rangePoly.coeffs[1]*rN
                    + ctx.rangePoly.coeffs[2]*aLoc + ctx.rangePoly.coeffs[3]*rN*aLoc
                    + ctx.rangePoly.coeffs[4]*rN*rN + ctx.rangePoly.coeffs[5]*aLoc*aLoc;
                double sx = c + colOff;
                double sy = (row + rowOffs[c]) - sY0;   // 条带内位置
                if (sx >= 0 && sx < sW - 1)
                    rowBuf[c] = useSinc ? sincInterp2D(sWin, sW, sYH, sx, sy, sincW, beta)
                                        : bilinearInterp2D(sWin, sW, sYH, sx, sy);
                else
                    rowBuf[c] = {0, 0};
            }
        }
        writer.writeRow(row, rowBuf);
        if (row % step == 0) QApplication::processEvents();
    }
    return true;
}

// ── 从内存条带内插单个像素 ──
static std::complex<float> interpFromStrip(const QVector<std::complex<float>>& strip,
    int sW, int sH, double sx, double sy,
    bool useSinc, int sincW, double beta)
{
    if (sx < 0 || sx >= sW - 1) return {0, 0};
    return useSinc ? sincInterp2D(strip, sW, sH, sx, sy, sincW, beta)
                   : bilinearInterp2D(strip, sW, sH, sx, sy);
}

// ── 从SoA burst缓冲区零拷贝提取条带视图 ──
static sar::ComplexSoAView extractSoaView(
    const sar::ComplexSoAView& soaBurst,
    int sW, int burstH, int burstRow0, int sY0, int sYH)
{
    if (sYH <= 0) return {};
    int localY0 = qMax(sY0, burstRow0) - burstRow0;
    int localY1 = qMin(sY0 + sYH, burstRow0 + burstH) - burstRow0;
    int h = localY1 - localY0;
    if (h <= 0) return {};
    int offset = localY0 * sW;
    return {soaBurst.re + offset, soaBurst.im + offset, sW * h};
}

// ── 计算一行的 slave 坐标 (方位偏移逐列: range-dependent azimuth) ──
struct RowCoords {
    QVector<double> sx;
    QVector<double> sy;   // 每列方位插值位置 (条带内坐标)
    int sY0, sYH;
};

static RowCoords computeRowCoords(int gRow, int mW, int mH, int sH,
    const RangePolynomial& rP, const AzimuthPolynomial& aP, int readR)
{
    RowCoords rc;
    rc.sx.resize(mW);
    rc.sy.resize(mW);
    double aLoc = (double)gRow / mH;

    double minRo = 1e9, maxRo = -1e9;
    QVector<double> rowOffs(mW);
    for (int c = 0; c < mW; ++c) {
        double rN = (double)c / mW;
        rowOffs[c] = aP.coeffs[0] + aP.coeffs[1]*aLoc + aP.coeffs[2]*rN;
        minRo = qMin(minRo, rowOffs[c]);
        maxRo = qMax(maxRo, rowOffs[c]);
    }
    int minRoI = (int)std::floor(minRo), maxRoI = (int)std::ceil(maxRo);

    rc.sY0 = gRow + minRoI - readR;
    rc.sYH = (maxRoI - minRoI) + readR * 2 + 1;
    if (rc.sY0 < 0) { rc.sYH += rc.sY0; rc.sY0 = 0; }
    if (rc.sY0 + rc.sYH > sH) rc.sYH = sH - rc.sY0;

    for (int c = 0; c < mW; ++c) {
        double rN = (double)c / mW;
        double colOff = rP.coeffs[0] + rP.coeffs[1]*rN + rP.coeffs[2]*aLoc
                      + rP.coeffs[3]*rN*aLoc + rP.coeffs[4]*rN*rN + rP.coeffs[5]*aLoc*aLoc;
        // 与 resampleNonTopsar 及粗配准测量构造一致:
        // master(c) ≈ slave(c + rangeOff) → sx = c + colOff
        rc.sx[c] = c + colOff;
        rc.sy[c] = (gRow + rowOffs[c]) - rc.sY0;
    }
    return rc;
}

// ── 并行重采样批次 ──
struct ResampleWorkItem {
    int gRowSrc, gRowOut;
    RangePolynomial rangePoly;
    AzimuthPolynomial aziPoly;
};

struct ResampleConfig {
    sar::ComplexSoAView fullBurstSoA;  // SoA burst缓冲区(已deramp)
    int sW, burstH, burstRow0, sH, mW, mH;
    int sincW; double beta;
    QVector<QVector<float>>* sincLUT;
};

static QVector<QPair<int, QVector<std::complex<float>>>> processResampleBatch(
    QVector<ResampleWorkItem> batch, ResampleConfig cfg)
{
    QVector<QPair<int, QVector<std::complex<float>>>> results;
    sar::ComplexSoA tempSoA;
    QVector<double> syBuf;

    for (const auto& w : batch) {
        auto rc = computeRowCoords(w.gRowSrc, cfg.mW, cfg.mH, cfg.sH,
            w.rangePoly, w.aziPoly, cfg.sincW);  // readR = sincW

        QVector<std::complex<float>> rowBuf(cfg.mW);
        if (rc.sYH <= 0) {
            rowBuf.fill({0, 0});
            results.append({w.gRowOut, std::move(rowBuf)});
            continue;
        }

        // 零拷贝SoA视图
        auto stripView = extractSoaView(cfg.fullBurstSoA, cfg.sW,
            cfg.burstH, cfg.burstRow0, rc.sY0, rc.sYH);
        int actualStripH = stripView.size / cfg.sW;
        if (actualStripH <= 0) {
            rowBuf.fill({0, 0});
            results.append({w.gRowOut, std::move(rowBuf)});
            continue;
        }

        // 条带被裁剪时校正 sy 原点 (实际条带起点可能晚于请求起点)
        const int actualStripY0 = std::max(rc.sY0, cfg.burstRow0);
        const double syShift = actualStripY0 - rc.sY0;

        // SoA直传: Horizontal SoA → tempSoA (SoA) → Vertical SoA → rowBuf (AoS)
        sar::sincInterp1D_Horizontal_SoA(stripView, cfg.sW, actualStripH, rc.sx,
            *cfg.sincLUT, cfg.sincW, tempSoA, cfg.mW);
        syBuf.resize(cfg.mW);
        for (int c = 0; c < cfg.mW; ++c)
            syBuf[c] = rc.sy[c] - syShift;   // 条带内位置 (裁剪原点校正)
        sar::sincInterp1D_Vertical_SoA(
            tempSoA.view(0, actualStripH * cfg.mW),
            actualStripH, cfg.mW, syBuf, *cfg.sincLUT, cfg.sincW, rowBuf.data());

        results.append({w.gRowOut, std::move(rowBuf)});
    }
    return results;
}

bool SincResampler::resampleTopsar(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int mW = ctx.data.masterWidth, mH = ctx.data.masterHeight;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    bool useSinc = (p.resamplingMethod == "Sinc");
    int sincW = p.sincWindowSize; double beta = p.sincBeta;
    int readR = useSinc ? sincW : 2;

    double prf    = (ctx.data.masterAzimuthFrequency > 0)
                         ? ctx.data.masterAzimuthFrequency : p.masterPrf;
    double kt     = ctx.data.slaveAzimuthFmRate;
    bool doDeramp = (std::abs(kt) > 1e-6) && (prf > 0);

    QVector<QVector<float>> sincLUT;
    bool useFastSinc = useSinc;
    if (useFastSinc) {
        initSincLUT(sincW, beta, sincLUT);
        qDebug() << QStringLiteral("[Step9] Sinc LUT ready (%1 levels)")
            .arg(sincLUT.size());
    }

    qDebug() << QStringLiteral("[Step9] band=%1 usingPrf=%2 Hz")
        .arg(ctx.masterBand->subSwath).arg(prf, 0, 'f', 2);

    if (ctx.burstResults.size() < N) {
        ctx.errorMessage = "SincResampler: burstResults not populated"; return false;
    }

    qDebug() << QStringLiteral("[Step9] TOPSAR resample %1x%2 %3bursts (no deburst) deramp=%4")
        .arg(mW).arg(mH).arg(N).arg(doDeramp ? "on" : "off");

    GdalSlcWriter writer;
    if (!writer.create(ctx.outputPath, mW, mH, 1)) {
        ctx.errorMessage = QStringLiteral("SincResampler: create output fail"); return false;
    }
    {
        void* geoRefHandle = ctx.useBurstCache
            ? (ctx.masterSdr ? ctx.masterSdr->datasetHandle() : nullptr)
            : (ctx.masterReader ? ctx.masterReader->datasetHandle() : nullptr);
        if (geoRefHandle)
            writer.copyGeoreferencing(geoRefHandle, QString());
    }

    int step = std::max(1, mH / 100);
    int nThreads = qBound(1, QThread::idealThreadCount(), 12);

    QElapsedTimer totalTimer; totalTimer.start();
    qint64 totalReadUs = 0, totalDerampUs = 0, totalSincUs = 0, totalWriteUs = 0;

    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        qDebug() << QStringLiteral("[Step9] burst %1/%2 reading full burst...").arg(b+1).arg(N);

        const auto& br = ctx.burstResults[b];
        int burstRow0 = b * L;

        // ── 辅影像 burst: 按 burstPairs 匹配结果取对应索引/起始行 ──
        int slaveBurstIdx = b;
        int sN = ctx.slaveBand->burstCount > 0 ? ctx.slaveBand->burstCount : N;
        if (ctx.burstPairs.size() == N
            && ctx.burstPairs[b].isValid
            && ctx.burstPairs[b].slaveBurstIdx >= 0
            && ctx.burstPairs[b].slaveBurstIdx < sN)
            slaveBurstIdx = ctx.burstPairs[b].slaveBurstIdx;
        int sL = ctx.slaveBand->linesPerBurst > 0 ? ctx.slaveBand->linesPerBurst : L;
        int slaveRow0 = slaveBurstIdx * sL;
        const auto& sStarts = ctx.slaveBand->burstStartLines;
        if (sStarts.size() > slaveBurstIdx)
            slaveRow0 = sStarts[slaveBurstIdx];

        // ── 方位偏移范围 vs 本 burst 窗口 ──
        // 大方位偏移 (如 12 天对沿轨差 238 行) 时源位置落在相邻 burst,
        // 窗口必须跨 burst 扩展; 缓存路径 (零拷贝 per-burst 视图) 无法跨 burst
        const double a0 = br.aziPoly.coeffs[0];
        const double a1 = br.aziPoly.coeffs[1];
        const double a2 = br.aziPoly.coeffs[2];
        const double aLoc0 = static_cast<double>(b * L) / mH;
        const double aLoc1 = static_cast<double>(qMin((b + 1) * L, mH)) / mH;
        const double v0 = a0 + a1 * aLoc0, v1 = a0 + a1 * aLoc1;
        const double minAzi = std::min(v0, v1) - std::abs(a2);
        const double maxAzi = std::max(v0, v1) + std::abs(a2);
        // 条带必须覆盖本 burst 全部输出行的源位置: k + aziOff ∈ [sincW, sL−sincW),
        // 即 首行源在条带内 (minAzi ≥ sincW) 且 末行源在条带内 ((L−1)+maxAzi < sL−sincW)
        const bool stripFitsInBurst = (minAzi - sincW >= 0)
            && (static_cast<double>(L - 1) + maxAzi + sincW < static_cast<double>(sL));

        // ── 获取 burst SoA 数据 ──
        qint64 readUs = 0, derampUs = 0;
        sar::ComplexSoA soaBurst;          // 拥有内存 (非缓存路径)
        sar::ComplexSoAView burstView;     // 最终使用的视图
        int winRow0 = slaveRow0;           // 窗口原点 (缓存路径 = burst 起始行)

        if (ctx.useBurstCache && ctx.slaveSdr && stripFitsInBurst) {
            // ── 缓存路径: 零拷贝, 数据已 deinterleaved + deramped ──
            burstView = ctx.slaveSdr->burstSoaView(slaveBurstIdx);
            if (!burstView.re || !burstView.im) {
                ctx.errorMessage = QStringLiteral("SincResampler: null cached burst view");
                return false;
            }
            if (burstView.size / sW <= 0 || burstView.size / sW > sL) {
                ctx.errorMessage = QStringLiteral("SincResampler: invalid cached burst view");
                return false;
            }
        } else if (ctx.useBurstCache && ctx.slaveSdr) {
            // ── SDR 直读扩展窗口 (缓存数据已由 TOPSARDeramp deramp) ──
            winRow0 = std::max(0, slaveRow0
                + static_cast<int>(std::floor(minAzi)) - sincW);
            const int readH = std::min(sH - winRow0,
                sL + static_cast<int>(std::ceil(maxAzi))
                    - static_cast<int>(std::floor(minAzi)) + 2 * sincW);
            if (readH <= 0) {
                ctx.errorMessage = QStringLiteral("SincResampler: slave burst out of bounds");
                return false;
            }
            QElapsedTimer rt; rt.start();
            QVector<std::complex<float>> fullBurst(static_cast<qsizetype>(sW) * readH);
            ctx.slaveSdr->readWindow(0, winRow0, sW, readH, fullBurst.data());
            readUs = rt.nsecsElapsed() / 1000;
            const int winH = readH;
            soaBurst.fromAos(fullBurst.constData(), fullBurst.size());
            fullBurst.clear();
            burstView = soaBurst.view(0, sW * winH);
            qDebug().nospace() << "[Step9] burst " << (b + 1)
                << " expanded window (SDR) winRow0=" << winRow0
                << " winH=" << winH
                << " aziOff=[" << minAzi << "," << maxAzi << "]";
        } else {
            // ── GDAL AoS 路径: 窗口按方位偏移扩展 + deramp ──
            // (缓存路径下 ctx.slaveReader 为 null, 必须走上面的 SDR 分支)
            winRow0 = std::max(0, slaveRow0
                + static_cast<int>(std::floor(minAzi)) - sincW);
            const int readH = std::min(sH - winRow0,
                sL + static_cast<int>(std::ceil(maxAzi))
                    - static_cast<int>(std::floor(minAzi)) + 2 * sincW);
            if (readH <= 0 || !ctx.slaveReader) {
                ctx.errorMessage = QStringLiteral("SincResampler: no slave reader for expanded window");
                return false;
            }
            QElapsedTimer rt; rt.start();
            QVector<std::complex<float>> fullBurst =
                ctx.slaveReader->readBandWindow(0, 0, winRow0, sW, readH);
            readUs = rt.nsecsElapsed() / 1000;
            if (fullBurst.isEmpty()) {
                ctx.errorMessage = QStringLiteral("SincResampler: readBandWindow empty");
                return false;
            }
            const int winH = fullBurst.size() / sW;
            rt.start();
            soaBurst.fromAos(fullBurst.constData(), fullBurst.size());
            if (doDeramp)
                sar::applyDeramp_SoA(soaBurst, sW, winH, winRow0, slaveBurstIdx, prf, kt);
            derampUs = rt.nsecsElapsed() / 1000;
            fullBurst.clear();
            burstView = soaBurst.view(0, sW * winH);
            qDebug().nospace() << "[Step9] burst " << (b + 1)
                << " expanded window (GDAL) winRow0=" << winRow0
                << " winH=" << winH
                << " aziOff=[" << minAzi << "," << maxAzi << "]";
        }

        // ── 构建工作项 + 分批 (输出行 = 主 burst 全部 L 行, 与窗口高度解耦) ──
        QVector<ResampleWorkItem> items;
        items.reserve(L);
        for (int r = 0; r < L; ++r)
            items.append({burstRow0 + r, burstRow0 + r,
                          br.rangePoly, br.aziPoly});

        int batchSz = qMax(1, (items.size() + nThreads * 4 - 1) / (nThreads * 4));
        QList<QVector<ResampleWorkItem>> batches;
        for (int i = 0; i < items.size(); i += batchSz) {
            QVector<ResampleWorkItem> batch;
            for (int j = i; j < qMin(i + batchSz, items.size()); ++j)
                batch.append(items[j]);
            batches.append(batch);
        }

        // ── SoA直传: 所有线程共享 burst 只读视图 ──
        ResampleConfig rcfg;
        rcfg.fullBurstSoA = burstView;
        rcfg.sW = sW; rcfg.burstH = burstView.size / sW; rcfg.burstRow0 = winRow0;
        rcfg.sH = sH;
        rcfg.mW = mW; rcfg.mH = mH;
        rcfg.sincW = sincW; rcfg.beta = beta;
        rcfg.sincLUT = &sincLUT;

        // ── Sinc插值并行 (纯SoA, 零拷贝) ──
        QElapsedTimer st; st.start();
        QList<QFuture<QVector<QPair<int, QVector<std::complex<float>>>>>> futures;
        for (int i = 0; i < batches.size(); ++i)
            futures.append(QtConcurrent::run(processResampleBatch, batches[i], rcfg));

        QVector<QPair<int, QVector<std::complex<float>>>> allRows;
        for (auto& f : futures)
            allRows += f.result();
        std::sort(allRows.begin(), allRows.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        qint64 sincUs = st.nsecsElapsed() / 1000;

        // ── 写出 ──
        st.start();
        for (const auto& row : allRows) {
            writer.writeRow(row.first, row.second);
            if (row.first % step == 0) QApplication::processEvents();
        }
        qint64 writeUs = st.nsecsElapsed() / 1000;
        QApplication::processEvents();

        totalReadUs += readUs; totalDerampUs += derampUs;
        totalSincUs += sincUs; totalWriteUs += writeUs;
        sincProfileLog(QStringLiteral("[Step9 PROFILE] burst %1 | read:%2ms deramp:%3ms sinc:%4ms write:%5ms")
            .arg(b+1).arg(readUs/1000).arg(derampUs/1000).arg(sincUs/1000).arg(writeUs/1000));
    }

    qint64 totalMs = totalTimer.nsecsElapsed() / 1000000;
    sincProfileLog(QStringLiteral("[Step9 TIMING] %1 bursts | read:%2ms deramp:%3ms sinc:%4ms write:%5ms total:%6ms")
        .arg(N).arg(totalReadUs/1000).arg(totalDerampUs/1000).arg(totalSincUs/1000).arg(totalWriteUs/1000).arg(totalMs));
    return true;
}

bool SincResampler::execute(PipelineContext& ctx) {
    if (ctx.isTopsar) return resampleTopsar(ctx);
    else return resampleNonTopsar(ctx);
}
