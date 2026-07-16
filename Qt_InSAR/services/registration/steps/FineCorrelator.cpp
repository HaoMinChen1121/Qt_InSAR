#include "FineCorrelator.h"
#include "../PipelineContext.h"
#include "algorithms/Correlation.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>

bool FineCorrelator::execute(PipelineContext& ctx) {
    const auto& p = *ctx.params;
    // 路线1无精配准; 路线2/3需要复数域相位相关
    if (p.route == RegRoute::Route1_OrbitFFT) return true;
    if (p.fineMethod != "FFT") return true;

    int sW = ctx.data.slaveWidth, sH = ctx.data.slaveHeight;
    int winSize = 128;
    int half = winSize / 2;
    int refined = 0, failed = 0;

    for (auto& pt : ctx.offsetPoints) {
        if (mCancelled) return false;

        int mX0 = pt.col - half, mY0 = pt.row - half;
        auto mWin = ctx.masterReader->readBandWindow(0, mX0, mY0, winSize, winSize);
        if (mWin.size() < winSize * winSize) { ++failed; continue; }

        int sX0 = pt.col + (int)pt.rangeOff - half;
        int sY0 = pt.row + (int)pt.aziOff - half;
        if (sX0 < 0) sX0 = 0; if (sY0 < 0) sY0 = 0;
        if (sX0 + winSize > sW) sX0 = sW - winSize;
        if (sY0 + winSize > sH) sY0 = sH - winSize;
        if (sX0 < 0 || sY0 < 0) { ++failed; continue; }
        auto sWin = ctx.slaveReader->readBandWindow(0, sX0, sY0, winSize, winSize);
        if (sWin.size() < winSize * winSize) { ++failed; continue; }

        int outRows = 2 * winSize - 1, outCols = 2 * winSize - 1;
        QVector<float> surf(outRows * outCols);
        float maxV = fftAmpCorrelate(mWin.data(), sWin.data(), surf.data(), winSize, winSize);

        double subDx, subDy;
        findPeakSubpixel(surf.data(), outRows, outCols, subDx, subDy);
        pt.rangeOff += subDx;
        pt.aziOff   += subDy;
        ++refined;
    }
    qDebug() << QStringLiteral("[Step7] FFT amp fine: %1 refined, %2 failed (win=%3)")
        .arg(refined).arg(failed).arg(winSize);
    return true;
}
