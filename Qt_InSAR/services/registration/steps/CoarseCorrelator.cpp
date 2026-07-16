#include "CoarseCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include <QDebug>
#include "dataaccess/impl/GdalSlcReader.h"

bool CoarseCorrelator::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int w = ctx.data.masterWidth, h = ctx.data.masterHeight;
    int N = ctx.data.burstCount, L = ctx.data.linesPerBurst;
    int nPerBurst = p.offsetPerBurst;
    int winSize = p.coarseWindowSize;
    int half = winSize / 2;

    bool useNcc = (p.route == RegRoute::Route2_NCC_FFTW);
    int searchHalf = useNcc ? p.coarseSearchWindow : 0;  // coarseSearchWindow = 搜索半径

    ctx.offsetPoints.clear();
    ctx.offsetPoints.reserve(N * nPerBurst);

    for (int b = 0; b < N; ++b) {
        if (mCancelled) return false;
        int startRow = b * L;
        for (int k = 0; k < nPerBurst; ++k) {
            OffsetPoint pt;
            pt.row = startRow + (k + 1) * L / (nPerBurst + 1);
            pt.col = static_cast<int>(w * (0.1 + 0.8 * k / (nPerBurst - 1.0)));
            for (const auto& io : ctx.initialOffsets)
                if (io.burstIndex == b) { pt.rangeOff = io.rangeOff; pt.aziOff = io.aziOff; break; }

            int mX0 = pt.col - half, mY0 = pt.row - half;
            auto mWin = ctx.masterReader->readBandWindow(0, mX0, mY0, winSize, winSize);
            if (mWin.size() < winSize * winSize) continue;

            if (useNcc) {
                // ── Route2: 幅度NCC搜索 ──
                int slaveWinSz = winSize + 2 * searchHalf;
                int sX0 = pt.col + (int)pt.rangeOff - half - searchHalf;
                int sY0 = pt.row + (int)pt.aziOff - half - searchHalf;
                if (sX0 < 0 || sY0 < 0 || sX0 + slaveWinSz > sW || sY0 + slaveWinSz > sH) continue;
                auto sWin = ctx.slaveReader->readBandWindow(0, sX0, sY0, slaveWinSz, slaveWinSz);
                if (sWin.size() < slaveWinSz * slaveWinSz) continue;

                int bestDx, bestDy;
                double subDx, subDy;
                double nccVal = nccCorrelate(mWin, sWin, winSize, winSize,
                                             slaveWinSz, slaveWinSz,
                                             bestDx, bestDy, subDx, subDy);

                // subDx/subDy已包含整像素+亚像素, 是相对于搜索区域中心的偏移
                pt.rangeOff += subDx;
                pt.aziOff   += subDy;
                pt.correlation = nccVal;
            } else {
                // ── Route1/3: FFT幅度域互相关 ──
                int sX0c = pt.col + (int)pt.rangeOff - half;
                int sY0c = pt.row + (int)pt.aziOff - half;
                if (sX0c < 0 || sY0c < 0 || sX0c + winSize > sW || sY0c + winSize > sH) continue;
                auto sWinC = ctx.slaveReader->readBandWindow(0, sX0c, sY0c, winSize, winSize);
                if (sWinC.size() < winSize * winSize) continue;

                int outRows = 2 * winSize - 1, outCols = 2 * winSize - 1;
                QVector<float> surf(outRows * outCols);
                float maxV = fftAmpCorrelate(mWin.data(), sWinC.data(), surf.data(), winSize, winSize);
                double subDx, subDy;
                findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
                pt.rangeOff += subDx;
                pt.aziOff   += subDy;
                pt.correlation = maxV;
            }
            ctx.offsetPoints.append(pt);
        }
    }

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
