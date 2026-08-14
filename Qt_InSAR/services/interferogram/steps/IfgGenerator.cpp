#include "IfgGenerator.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include "dataaccess/ISarProduct.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Step 1: 复干涉图 + 相干性 + 相位 — 多视 + 逐 burst 干涉
// (TOPSAR: 逐 burst 写 3 波段临时块, 拼接/方位校正在 TopsarDeburst;
//  非 TOPSAR: 直写 deburst 输出)
bool IfgGenerator::execute(IfgPipelineContext& ctx)
{
    qDebug() << "[Ifg] stageInterferogram: master=" << ctx.masterPath.left(80);
    qDebug() << "[Ifg] stageInterferogram: slave=" << ctx.slavePath;

    // Master: 优先用 SentinelDataReader (快速 ZIP 读取), 回退 GdalSlcReader
    SentinelDataReader mSdr;
    GdalSlcReader mReader, sReader;
    bool useSdr = !ctx.masterZip.isEmpty() && !ctx.masterEntry.isEmpty();
    if (useSdr) {
        SarBandDescriptor dummyBand;
        dummyBand.burstCount = 0;  // SDR 不用于 burst 缓存, 仅用于 open
        if (!mSdr.open(ctx.masterZip, ctx.masterEntry, dummyBand)) {
            qWarning() << "[Ifg] SDR open failed, fallback to GdalSlcReader";
            useSdr = false;
        }
    }
    if (!useSdr) {
        if (!mReader.open(ctx.masterPath)) {
            ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open master");
            return false;
        }
    }
    if (!sReader.open(ctx.slavePath)) {
        ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open slave");
        return false;
    }

    int realW = useSdr ? mSdr.width() : mReader.width();
    int realH = useSdr ? mSdr.height() : mReader.height();
    int rgLooks = ctx.params->rangeLooks;
    int azLooks = ctx.params->azimuthLooks;
    int outW = realW / rgLooks;
    if (outW < 1) { ctx.errorMessage = "IfgGenerator: outW < 1"; return false; }

    qDebug() << "[Ifg-CK1] master" << realW << "x" << realH
             << "slave" << sReader.width() << "x" << sReader.height();

    bool isTopsar = ctx.burstInfo && ctx.burstInfo->burstCount > 1;

    GDALDriverH driver = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    QVector<std::complex<float>> rowComplex(outW);
    QVector<float> rowPhase(outW);
    QVector<float> rowCoh(outW);
    int cohWindow = 5;

    if (isTopsar) {
        // ── TOPSAR: 逐 burst 完整多视 → 临时块 ──
        // 4 波段 Float32 (单次 GDALCreate, 不依赖 GDALAddBand — 旧版 GDAL 不支持):
        //   band1=ifg 实部, band2=ifg 虚部, band3=coh, band4=phase
        int N = ctx.burstInfo->burstCount;
        int L = ctx.burstInfo->linesPerBurst;
        int burstOutH = L / azLooks;   // 完整 burst (重叠裁剪由 TopsarDeburst 完成)
        if (burstOutH < 1) { ctx.errorMessage = "IfgGenerator: burstOutH < 1"; return false; }

        QVector<float> rowRe(outW);
        QVector<float> rowIm(outW);

        for (int b = 0; b < N; ++b) {
            if (mCancelled) return false;

            int burstRow0 = b * L;
            int readRow0 = burstRow0 - cohWindow;
            int readH = L + cohWindow * 2;
            if (readRow0 < 0) { readH += readRow0; readRow0 = 0; }
            if (readRow0 + readH > realH) readH = realH - readRow0;

            QVector<std::complex<float>> mBurst(realW * readH);
            QVector<std::complex<float>> sBurst;
            if (useSdr)
                mSdr.readWindow(0, readRow0, realW, readH, mBurst.data());
            else
                mBurst = mReader.readBandWindow(0, 0, readRow0, realW, readH);
            sBurst = sReader.readBandWindow(0, 0, readRow0, realW, readH);
            int actualH = std::min(mBurst.size() / realW, sBurst.size() / realW);

            QString blkPath = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 1);
            GDALDatasetH hBlk = GDALCreate(driver, blkPath.toUtf8().constData(),
                                           outW, burstOutH, 4, GDT_Float32, nullptr);
            if (!hBlk) {
                ctx.errorMessage = QStringLiteral("IfgGenerator: cannot create burst block %1").arg(blkPath);
                return false;
            }
            GDALSetGeoTransform(hBlk, gt);

            qDebug() << "[Ifg] burst" << (b+1) << "/" << N
                     << "blockRows=" << burstOutH;

            for (int j = 0; j < burstOutH; ++j) {
                int srcRow = burstRow0 + j * azLooks;
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
                    rowRe[col] = static_cast<float>(ifg.real());
                    rowIm[col] = static_cast<float>(ifg.imag());
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

                GDALRasterIO(GDALGetRasterBand(hBlk, 1), GF_Write, 0, j, outW, 1,
                    rowRe.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hBlk, 2), GF_Write, 0, j, outW, 1,
                    rowIm.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hBlk, 3), GF_Write, 0, j, outW, 1,
                    rowCoh.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hBlk, 4), GF_Write, 0, j, outW, 1,
                    rowPhase.data(), outW, 1, GDT_Float32, 0, 0);
            }
            GDALClose(hBlk);
        }

        ctx.outWidth = outW;
        // ctx.outHeight 由 TopsarDeburst 计算
    } else {
        // ── 非 TOPSAR: 直写 deburst 输出 ──
        int outH = realH / azLooks;
        if (outH < 1) { ctx.errorMessage = "IfgGenerator: outH < 1"; return false; }

        QString base = ctx.deburstOutputBase;
        QDir().mkpath(QFileInfo(base).absolutePath());
        QString ifgPath  = base + "_ifg.tif";
        QString cohPath  = base + "_coh.tif";
        QString phasePath = base + "_phase.tif";

        GDALDatasetH hIfg = GDALCreate(driver, ifgPath.toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
        GDALDatasetH hCoh = GDALCreate(driver, cohPath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
        GDALDatasetH hPh  = GDALCreate(driver, phasePath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
        if (!hIfg || !hCoh || !hPh) { ctx.errorMessage = "IfgGenerator: cannot create output"; return false; }
        GDALSetGeoTransform(hIfg, gt); GDALSetGeoTransform(hCoh, gt); GDALSetGeoTransform(hPh, gt);

        for (int row = 0; row < outH; ++row) {
            if (mCancelled) { GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh); return false; }
            int srcRow = row * azLooks;
            int readH = azLooks + cohWindow * 2;
            int row0 = srcRow - cohWindow;

            QVector<std::complex<float>> mData(realW * readH);
            if (useSdr)
                mSdr.readWindow(0, row0, realW, readH, mData.data());
            else
                mData = mReader.readBandWindow(0, 0, row0, realW, readH);
            auto sData = sReader.readBandWindow(0, 0, row0, realW, readH);
            int actualH = std::min(mData.size() / realW, sData.size() / realW);
            if (actualH == 0) {
                rowComplex.fill(std::complex<float>(0,0));
                rowPhase.fill(0); rowCoh.fill(0);
                GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, reinterpret_cast<CFloat32*>(rowComplex.data()), outW, 1, GDT_CFloat32, 0, 0);
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

            GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, reinterpret_cast<CFloat32*>(rowComplex.data()), outW, 1, GDT_CFloat32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
            GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, row, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
        }

        GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);
        ctx.outWidth  = outW;
        ctx.outHeight = outH;
    }

    qDebug() << "[Ifg] stageInterferogram SUCCESS";
    return true;
}
