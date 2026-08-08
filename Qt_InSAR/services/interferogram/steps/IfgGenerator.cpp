#include "IfgGenerator.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void computeBurstDiscard(int N, int linesPerBurst, double prf,
    const QVector<QDateTime>& burstTimes,
    QVector<int>& discardTop, QVector<int>& discardBottom)
{
    discardTop.fill(0, N);
    discardBottom.fill(0, N);
    if (N < 2) return;

    if (burstTimes.size() >= N) {
        for (int b = 0; b < N - 1; ++b) {
            double dt = burstTimes[b].msecsTo(burstTimes[b+1]) / 1000.0;
            double burstDur = linesPerBurst / prf;
            double overlapTime = burstDur - dt;
            if (overlapTime < 0) overlapTime = 0;
            int overlapLines = (int)(overlapTime * prf + 0.5);
            int half = overlapLines / 2;
            discardBottom[b] = half;
            discardTop[b + 1] = half;
        }
    } else {
        for (int b = 0; b < N - 1; ++b) {
            discardBottom[b] = 40;
            discardTop[b + 1] = 40;
        }
    }
}

bool IfgGenerator::execute(IfgPipelineContext& ctx)
{
    qDebug() << "[Ifg] stageInterferogram: master=" << ctx.masterPath.left(80);
    qDebug() << "[Ifg] stageInterferogram: slave=" << ctx.slavePath;

    GdalSlcReader mReader, sReader;
    if (!mReader.open(ctx.masterPath)) {
        ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open master");
        return false;
    }
    if (!sReader.open(ctx.slavePath)) {
        ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open slave");
        return false;
    }

    int realW = mReader.width();
    int realH = mReader.height();
    int rgLooks = ctx.params->rangeLooks;
    int azLooks = ctx.params->azimuthLooks;
    int outW = realW / rgLooks;
    if (outW < 1) { ctx.errorMessage = "IfgGenerator: outW < 1"; return false; }

    qDebug() << "[Ifg-CK1] master" << realW << "x" << realH
             << "slave" << sReader.width() << "x" << sReader.height();

    bool isTopsar = ctx.burstInfo && ctx.burstInfo->burstCount > 1;
    int outH;
    QVector<int> discardTop, discardBottom, burstOutOffsets;

    if (isTopsar) {
        int N = ctx.burstInfo->burstCount;
        int L = ctx.burstInfo->linesPerBurst;
        double prf = ctx.burstInfo->azimuthFrequency > 0
            ? ctx.burstInfo->azimuthFrequency : 486.0;

        computeBurstDiscard(N, L, prf, ctx.burstInfo->burstAzimuthTimes,
            discardTop, discardBottom);

        burstOutOffsets.resize(N);
        outH = 0;
        for (int b = 0; b < N; ++b) {
            burstOutOffsets[b] = outH;
            int validLines = L - discardTop[b] - discardBottom[b];
            outH += validLines / azLooks;
        }
        qDebug() << "[Ifg] TOPSAR deburst" << N << "bursts L=" << L
                 << "prf=" << prf << "outH=" << outH;
    } else {
        outH = realH / azLooks;
    }

    if (outH < 1) { ctx.errorMessage = "IfgGenerator: outH < 1"; return false; }

    QString ifgBase = ctx.ifgOutputBase;
    QDir().mkpath(QFileInfo(ifgBase).absolutePath());
    QString ifgPath  = ifgBase + "_ifg.tif";
    QString cohPath  = ifgBase + "_coh.tif";
    QString phasePath = ifgBase + "_phase.tif";

    GDALDriverH driver = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALDatasetH hIfg = GDALCreate(driver, ifgPath.toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hCoh = GDALCreate(driver, cohPath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    GDALDatasetH hPh  = GDALCreate(driver, phasePath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    if (!hIfg || !hCoh || !hPh) { ctx.errorMessage = "IfgGenerator: cannot create output"; return false; }
    GDALSetGeoTransform(hIfg, gt); GDALSetGeoTransform(hCoh, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowComplex(outW);
    QVector<float> rowPhase(outW);
    QVector<float> rowCoh(outW);
    int cohWindow = 5;

    if (isTopsar) {
        int N = ctx.burstInfo->burstCount;
        int L = ctx.burstInfo->linesPerBurst;

        for (int b = 0; b < N; ++b) {
            if (mCancelled) { GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh); return false; }

            int burstRow0 = b * L;
            int validRow0 = burstRow0 + discardTop[b];
            int validRow1 = burstRow0 + L - 1 - discardBottom[b];
            int validRows = validRow1 - validRow0 + 1;
            int burstOutH = validRows / azLooks;
            if (burstOutH <= 0) continue;

            int readRow0 = burstRow0 - cohWindow;
            int readH = L + cohWindow * 2;
            if (readRow0 < 0) { readH += readRow0; readRow0 = 0; }
            if (readRow0 + readH > realH) readH = realH - readRow0;

            auto mBurst = mReader.readBandWindow(0, 0, readRow0, realW, readH);
            auto sBurst = sReader.readBandWindow(0, 0, readRow0, realW, readH);
            int actualH = std::min(mBurst.size() / realW, sBurst.size() / realW);

            qDebug() << "[Ifg] burst" << (b+1) << "/" << N
                     << "validRows=" << validRows << "outRows=" << burstOutH;

            for (int outLocal = 0; outLocal < burstOutH; ++outLocal) {
                int outRow = burstOutOffsets[b] + outLocal;
                int srcRow = validRow0 + outLocal * azLooks;
                int rowOff = srcRow - readRow0;

                for (int col = 0; col < outW; ++col) {
                    int srcCol = col * rgLooks;

                    std::complex<double> mAvg(0, 0), sAvg(0, 0);
                    int nPix = 0;
                    for (int ar = 0; ar < azLooks; ++ar) {
                        for (int ac = 0; ac < rgLooks; ++ac) {
                            int idx = (rowOff + ar) * realW + (srcCol + ac);
                            if (idx >= 0 && idx < actualH * realW) {
                                mAvg += std::complex<double>(mBurst[idx].real(), mBurst[idx].imag());
                                sAvg += std::complex<double>(sBurst[idx].real(), sBurst[idx].imag());
                                ++nPix;
                            }
                        }
                    }
                    if (nPix > 0) { mAvg /= nPix; sAvg /= nPix; }

                    std::complex<double> ifg = mAvg * std::conj(sAvg);
                    rowComplex[col] = std::complex<float>(ifg.real(), ifg.imag());
                    rowPhase[col] = std::atan2(ifg.imag(), ifg.real());

                    std::complex<double> crossSum(0, 0);
                    double magM = 0, magS = 0;
                    for (int wr = -cohWindow/2; wr <= cohWindow/2; ++wr) {
                        for (int wc = -cohWindow/2; wc <= cohWindow/2; ++wc) {
                            int sc = srcCol + cohWindow/2 + wc;
                            int sr = rowOff + cohWindow/2 + wr;
                            if (sc >= 0 && sc < realW && sr >= 0 && sr < actualH) {
                                int idx = sr * realW + sc;
                                auto mv = mBurst[idx]; auto sv = sBurst[idx];
                                crossSum += std::complex<double>(mv.real(), mv.imag())
                                    * std::complex<double>(sv.real(), -sv.imag());
                                magM += mv.real()*mv.real() + mv.imag()*mv.imag();
                                magS += sv.real()*sv.real() + sv.imag()*sv.imag();
                            }
                        }
                    }
                    double denom = std::sqrt(std::max(1e-15, magM * magS));
                    rowCoh[col] = static_cast<float>(std::abs(crossSum) / denom);
                }

                GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, outRow, outW, 1, rowComplex.data(), outW, 1, GDT_CFloat32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, outRow, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
                GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, outRow, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
            }
        }
    } else {
        for (int row = 0; row < outH; ++row) {
            if (mCancelled) { GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh); return false; }
            int srcRow = row * azLooks;
            int readH = azLooks + cohWindow * 2;
            int row0 = srcRow - cohWindow;

            auto mData = mReader.readBandWindow(0, 0, row0, realW, readH);
            auto sData = sReader.readBandWindow(0, 0, row0, realW, readH);
            int actualH = std::min(mData.size() / realW, sData.size() / realW);
            if (actualH == 0) {
                rowComplex.fill(std::complex<float>(0,0));
                rowPhase.fill(0); rowCoh.fill(0);
                GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, rowComplex.data(), outW, 1, GDT_CFloat32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
                GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, row, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
                continue;
            }

            int rowOff = cohWindow + (row0 < 0 ? row0 : 0);

            for (int col = 0; col < outW; ++col) {
                int srcCol = col * rgLooks;

                std::complex<double> mAvg(0, 0), sAvg(0, 0);
                for (int ar = 0; ar < azLooks; ++ar) {
                    for (int ac = 0; ac < rgLooks; ++ac) {
                        int idx = (rowOff + ar) * realW + (srcCol + ac);
                        if (idx >= 0 && idx < actualH * realW) {
                            mAvg += std::complex<double>(mData[idx].real(), mData[idx].imag());
                            sAvg += std::complex<double>(sData[idx].real(), sData[idx].imag());
                        }
                    }
                }
                int nPix = azLooks * rgLooks;
                mAvg /= nPix;
                sAvg /= nPix;

                std::complex<double> ifg = mAvg * std::conj(sAvg);
                rowComplex[col] = std::complex<float>(ifg.real(), ifg.imag());
                rowPhase[col] = std::atan2(ifg.imag(), ifg.real());

                std::complex<double> crossSum(0, 0);
                double magM = 0, magS = 0;
                for (int wr = -cohWindow/2; wr <= cohWindow/2; ++wr) {
                    for (int wc = -cohWindow/2; wc <= cohWindow/2; ++wc) {
                        int sc = srcCol + cohWindow/2 + wc;
                        int sr = rowOff + cohWindow/2 + wr;
                        if (sc >= 0 && sc < realW && sr >= 0 && sr < actualH) {
                            int idx = sr * realW + sc;
                            auto mv = mData[idx]; auto sv = sData[idx];
                            crossSum += std::complex<double>(mv.real(), mv.imag())
                                * std::complex<double>(sv.real(), -sv.imag());
                            magM += mv.real()*mv.real() + mv.imag()*mv.imag();
                            magS += sv.real()*sv.real() + sv.imag()*sv.imag();
                        }
                    }
                }
                double denom = std::sqrt(std::max(1e-15, magM * magS));
                rowCoh[col] = static_cast<float>(std::abs(crossSum) / denom);
            }

            GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, rowComplex.data(), outW, 1, GDT_CFloat32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
            GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, row, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
        }
    }

    GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);
    ctx.outWidth  = outW;
    ctx.outHeight = outH;
    qDebug() << "[Ifg] stageInterferogram SUCCESS";
    return true;
}
