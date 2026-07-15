#include "InterferogramServiceImpl.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalInterferogramWriter.h"
#include "dataaccess/impl/GdalDemReader.h"
#include "dataaccess/impl/QsarIO.h"
#include "dataaccess/SarProductFactory.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QScopedPointer>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

InterferogramServiceImpl::InterferogramServiceImpl(QObject* parent)
    : IInterferogramService(parent) {}

void InterferogramServiceImpl::setParams(const InterferogramParams& p) { mParams = p; }
InterferogramParams InterferogramServiceImpl::params() const { return mParams; }
void InterferogramServiceImpl::cancel() { mCancelled = true; }
bool InterferogramServiceImpl::isRunning() const { return mRunning; }

void InterferogramServiceImpl::execute()
{
    mRunning = true;
    mCancelled = false;

    GDALAllRegister();

    if (mParams.masterQsarPath.isEmpty() || mParams.slaveQsarPath.isEmpty()) {
        emit errorOccurred(QStringLiteral("请先选择主影像和辅影像产品"));
        emit finished(false, QString());
        mRunning = false;
        return;
    }

    // 辅影像: 读取 QSAR (registered.qsar)
    QsarProduct slaveQsar = QsarIO::read(mParams.slaveQsarPath);
    if (slaveQsar.bands.isEmpty()) {
        emit errorOccurred(QStringLiteral("辅影像QSAR无波段数据"));
        emit finished(false, QString()); mRunning = false; return;
    }

    // 主影像: 支持 .zip/.SAFE (原始SLC) 或 .qsar
    QsarProduct masterQsar;
    QScopedPointer<ISarProduct> masterProduct;
    if (mParams.masterQsarPath.endsWith(".qsar", Qt::CaseInsensitive)) {
        masterQsar = QsarIO::read(mParams.masterQsarPath);
    } else {
        // .zip / .SAFE → 打开 Sentinel1Product 获取波段信息
        masterProduct.reset(createSarProduct(mParams.masterQsarPath));
        if (!masterProduct || !masterProduct->open(mParams.masterQsarPath)) {
            emit errorOccurred(QStringLiteral("无法打开主影像产品"));
            emit finished(false, QString()); mRunning = false; return;
        }
        masterQsar.sourceMaster = masterProduct->sensorInfo().missionId;
        mParams.incidenceAngle = masterProduct->sensorInfo().incidenceAngleMid;
        mParams.wavelength = masterProduct->sensorInfo().wavelength;
        mParams.nearRange = masterProduct->sensorInfo().nearRange;
        mParams.rangeSpacing = masterProduct->sensorInfo().rangeSpacing;
        mParams.prf = masterProduct->sensorInfo().prf;
        for (const auto& b : masterProduct->bands()) {
            QsarBand qb;
            qb.subSwath = b.subSwath;
            qb.polarization = b.polarization;
            qb.file = b.rasterPath;
            qb.width  = b.rasterSize.width();
            qb.height = b.rasterSize.height();
            masterQsar.bands.append(qb);
        }
    }

    if (masterQsar.bands.isEmpty()) {
        emit errorOccurred(QStringLiteral("主影像无波段数据"));
        emit finished(false, QString()); mRunning = false; return;
    }

    // 按 subSwath + polarization 配对波段
    struct BandPair { QsarBand master, slave; };
    QVector<BandPair> pairs;
    for (const auto& mb : masterQsar.bands) {
        for (const auto& sb : slaveQsar.bands) {
            if (mb.subSwath == sb.subSwath && mb.polarization == sb.polarization) {
                pairs.append({mb, sb}); break;
            }
        }
    }
    emit progressChanged(0, QStringLiteral("波段配对: %1对").arg(pairs.size()));

    QString outputDir = mParams.outputDir;
    if (outputDir.isEmpty()) outputDir = QFileInfo(mParams.masterQsarPath).absolutePath();
    QString prefix = mParams.outputPrefix;

    int succeeded = 0;
    // 准备子目录
    QString ifgDir  = outputDir + "/ifg";
    QString flatDir = outputDir + "/flat";
    QString diffDir = outputDir + "/diff";
    QDir().mkpath(ifgDir);
    QDir().mkpath(flatDir);
    QDir().mkpath(diffDir);

    QsarProduct qsar;
    qsar.productType = "Interferogram";
    qsar.created = QDateTime::currentDateTime().toString(Qt::ISODate);
    qsar.sourceMaster = mParams.masterProductDisplay;
    qsar.sourceSlave  = mParams.slaveProductDisplay;
    qsar.outputPrefix = mParams.outputPrefix;
    qsar.stages << "ifg";

    for (int i = 0; i < pairs.size(); ++i) {
        if (mCancelled) break;
        QString sw = pairs[i].master.subSwath;
        QString pol = pairs[i].master.polarization;
        QString pairName = QStringLiteral("%1_%2").arg(sw).arg(pol);
        int basePct = i * 100 / pairs.size();
        emit progressChanged(basePct, QStringLiteral("处理 %1/%2: %3").arg(i+1).arg(pairs.size()).arg(pairName));

        int w = pairs[i].master.width;
        int h = pairs[i].master.height;

        QsarBand qb;
        qb.subSwath = sw; qb.polarization = pol;
        qb.width = w / mParams.rangeLooks;
        qb.height = h / mParams.azimuthLooks;

        // === Stage 1: 干涉图 ===
        emit progressChanged(basePct + 5, pairName + QStringLiteral(": 干涉图生成..."));
        QString ifgBase = ifgDir + "/" + pairName;
        if (!stageInterferogram(pairs[i].master.file, pairs[i].slave.file,
                ifgBase, w, h, mParams.rangeLooks, mParams.azimuthLooks)) {
            qWarning() << "[Ifg] Stage 1 failed:" << pairName;
            continue;
        }
        qb.file = QStringLiteral("ifg/%1_ifg.tif").arg(pairName);
        qb.ifgFile  = qb.file;
        qb.cohFile  = QStringLiteral("ifg/%1_coh.tif").arg(pairName);
        qb.phaseFile = QStringLiteral("ifg/%1_phase.tif").arg(pairName);

        // === Stage 2: 平地效应 ===
        if (mParams.enableFlatEarth) {
            emit progressChanged(basePct + 35, pairName + QStringLiteral(": 平地效应去除..."));
            double incRad = mParams.incidenceAngle * M_PI / 180.0;
            if (stageFlatEarth(ifgBase + "_ifg.tif", flatDir + "/" + pairName, w, h, mParams.wavelength, mParams.nearRange, mParams.rangeSpacing, mParams.prf, incRad, mParams.baselinePar)) {
                if (qsar.stages.isEmpty() || qsar.stages.last() != "flat")
                    qsar.stages << "flat";
                qb.flatFile = QStringLiteral("flat/%1_flat.tif").arg(pairName);
                qb.flatPhaseFile = QStringLiteral("flat/%1_flat_phase.tif").arg(pairName);
            }
        }

        // === Stage 3: 差分 ===
        if (mParams.enableDifferential && !mParams.demPath.isEmpty()) {
            emit progressChanged(basePct + 65, pairName + QStringLiteral(": 差分干涉..."));
            QString flatSrc = qb.flatFile.isEmpty() ? ifgBase + "_ifg.tif"
                : flatDir + "/" + pairName + "_flat.tif";
            double incRad = mParams.incidenceAngle * M_PI / 180.0;
            if (stageDifferential(flatSrc, mParams.demPath, diffDir + "/" + pairName, w, h, mParams.wavelength, mParams.nearRange, mParams.rangeSpacing, incRad, mParams.baselinePerp)) {
                if (qsar.stages.isEmpty() || qsar.stages.last() != "diff")
                    qsar.stages << "diff";
                qb.diffFile = QStringLiteral("diff/%1_diff.tif").arg(pairName);
                qb.diffPhaseFile = QStringLiteral("diff/%1_diff_phase.tif").arg(pairName);
            }
        }

        qsar.bands.append(qb);
        ++succeeded;
        basePct = (i + 1) * 100 / pairs.size();
        emit progressChanged(basePct, QStringLiteral("完成 %1/%2").arg(i+1).arg(pairs.size()));
    }

    QString qsarPath = outputDir + "/" + mParams.outputPrefix + ".qsar";
    QsarIO::write(qsarPath, qsar);

    emit progressChanged(100, QStringLiteral("干涉图生成完成 (%1/%2对)").arg(succeeded).arg(pairs.size()));
    emit finished(succeeded > 0, qsarPath);
    mRunning = false;
}

// ── Stage 1: 多视 + 干涉图 + 相干性 ──
bool InterferogramServiceImpl::stageInterferogram(
    const QString& masterPath, const QString& slavePath,
    const QString& outBase, int width, int height,
    int rgLooks, int azLooks)
{
    qDebug() << "[Ifg] stageInterferogram: master=" << masterPath.left(80);
    qDebug() << "[Ifg] stageInterferogram: slave=" << slavePath;

    GdalSlcReader mReader, sReader;
    if (!mReader.open(masterPath)) {
        qWarning() << "[Ifg] cannot open master:" << masterPath;
        return false;
    }
    if (!sReader.open(slavePath)) {
        qWarning() << "[Ifg] cannot open slave:" << slavePath;
        return false;
    }

    qDebug() << "[Ifg-CK1] master" << mReader.width() << "x" << mReader.height()
             << "slave" << sReader.width() << "x" << sReader.height();

    // 用实际读取尺寸而非传入参数（QSAR band 可能未设 rasterSize）
    int realW = mReader.width();
    int realH = mReader.height();
    int outW = realW / rgLooks;
    int outH = realH / azLooks;
    if (outW < 1 || outH < 1) return false;

    QDir().mkpath(QFileInfo(outBase).absolutePath());
    QString ifgPath  = outBase + "_ifg.tif";
    QString cohPath  = outBase + "_coh.tif";
    QString phasePath = outBase + "_phase.tif";

    // 创建三个独立文件
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALDatasetH hIfg = GDALCreate(driver, ifgPath.toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hCoh = GDALCreate(driver, cohPath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    GDALDatasetH hPh  = GDALCreate(driver, phasePath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    if (!hIfg || !hCoh || !hPh) { qWarning() << "[Ifg] cannot create output"; return false; }
    GDALSetGeoTransform(hIfg, gt); GDALSetGeoTransform(hCoh, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowComplex(outW);
    QVector<float> rowPhase(outW);
    QVector<float> rowCoh(outW);
    int cohWindow = 5;

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

    GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);
    qDebug() << "[Ifg] stageInterferogram SUCCESS";
    return true;
}

// ── Stage 2: 平地相位去除 (椭球面近似) ──
bool InterferogramServiceImpl::stageFlatEarth(
    const QString& ifgPath, const QString& outBase,
    int width, int height, double wavelength,
    double nearRange, double rangeSpacing, double prf,
    double incidenceAngleRad, double Bpar)
{
    Q_UNUSED(height);
    Q_UNUSED(prf);

    GdalSlcReader reader;
    if (!reader.open(ifgPath)) return false;
    int w = reader.width(), h = reader.height();

    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    QString flatPath = outBase + "_flat.tif";
    QString phasePath = outBase + "_flat_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, flatPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) return false;
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);
    double Re = 6378137.0, H = 800000.0;

    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            double R = nearRange + col * rangeSpacing;
            double theta = incidenceAngleRad;  // 从主产品标注XML读取
            double phiFlat = 0;
            if (R > 0) {
                phiFlat = -4.0 * M_PI / wavelength * Bpar * std::sin(theta);
            }
            float c = std::cos(static_cast<float>(phiFlat));
            float s = std::sin(static_cast<float>(phiFlat));
            auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0,0);
            auto flatVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = flatVal;
            rowPhase[col] = std::atan2(flatVal.imag(), flatVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1, rowBuf.data(), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1, rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh);
    return true;
}

// ── Stage 3: 差分干涉 ──
bool InterferogramServiceImpl::stageDifferential(
    const QString& flatPath, const QString& demPath, const QString& outBase,
    int width, int height, double wavelength,
    double nearRange, double rangeSpacing,
    double incidenceAngleRad, double Bperp)
{
    Q_UNUSED(width); Q_UNUSED(height);

    GdalSlcReader reader;
    if (!reader.open(flatPath)) return false;
    int w = reader.width(), h = reader.height();

    GdalDemReader dem;
    if (!dem.open(demPath)) { reader.close(); return false; }
    int demW = dem.width(), demH = dem.height();

    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    QString diffPath = outBase + "_diff.tif";
    QString phasePath = outBase + "_diff_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, diffPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) { reader.close(); dem.close(); return false; }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);
    QVector<float> demRowData(demW);
    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close(); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        int demRow = row * demH / h;
        demRow = qBound(0, demRow, demH - 1);
        // 逐行读DEM（避免逐像素读的性能灾难）
        auto demLine = dem.readElevationWindow(0, demRow, demW, 1);
        if (demLine.size() >= demW) demRowData = demLine;

        for (int col = 0; col < w; ++col) {
            double R = nearRange + col * rangeSpacing;
            double theta = incidenceAngleRad;
            int demCol = col * demW / w;
            demCol = qBound(0, demCol, demW - 1);
            double hDem = static_cast<double>(demRowData[demCol]);
            if (hDem < -1000.0) hDem = 0.0;   // nodata → 海平面
            double phiTopo = -4.0 * M_PI / wavelength * Bperp * hDem / (R * std::sin(theta));

            float c = std::cos(static_cast<float>(phiTopo));
            float s = std::sin(static_cast<float>(phiTopo));
            auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            auto diffVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = diffVal;
            rowPhase[col] = std::atan2(diffVal.imag(), diffVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1, rowBuf.data(), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1, rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close();
    return true;
}
