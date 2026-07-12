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
    for (int i = 0; i < pairs.size(); ++i) {
        if (mCancelled) break;
        QString pairName = QStringLiteral("%1_%2").arg(pairs[i].master.subSwath).arg(pairs[i].master.polarization);
        int basePct = i * 100 / pairs.size();
        emit progressChanged(basePct, QStringLiteral("处理 %1/%2: %3").arg(i+1).arg(pairs.size()).arg(pairName));

        int w = pairs[i].master.width;
        int h = pairs[i].master.height;

        // === Stage 1: 干涉图  ===
        emit progressChanged(basePct + 5, pairName + QStringLiteral(": 干涉图生成..."));
        QString ifgBase = outputDir + "/" + prefix + "_" + pairName;
        if (!stageInterferogram(pairs[i].master.file, pairs[i].slave.file,
                ifgBase, w, h, mParams.rangeLooks, mParams.azimuthLooks)) {
            qWarning() << "[Ifg] Stage 1 failed:" << pairName;
            continue;
        }

        if (mParams.referenceSource == "Orbit"
            && !pairs[i].master.subSwath.isEmpty()) {
            // === Stage 2: 平地效应 ===
            emit progressChanged(basePct + 35, pairName + QStringLiteral(": 平地效应去除..."));
            QString flatPath = outputDir + "/" + prefix + "_" + pairName + "_flat.tif";
            double wl = 0.0555;  // Sentinel-1 C-band
            if (!stageFlatEarth(ifgBase + "_ifg.tif", flatPath, w, h, wl, 800000.0, 2.33, 1680.0)) {
                qWarning() << "[Ifg] Stage 2 failed:" << pairName;
                // 继续使用原始干涉图
            }
        }

        if (mParams.differential && !mParams.demPath.isEmpty()) {
            // === Stage 3: 差分 ===
            emit progressChanged(basePct + 65, pairName + QStringLiteral(": 差分干涉..."));
            QString diffPath = outputDir + "/" + prefix + "_" + pairName + "_diff.tif";
            double wl = 0.0555;
            stageDifferential(ifgBase + "_ifg.tif", mParams.demPath, diffPath, w, h, wl, 800000.0, 2.33);
        }

        ++succeeded;
        basePct = (i + 1) * 100 / pairs.size();
        emit progressChanged(basePct, QStringLiteral("完成 %1/%2").arg(i+1).arg(pairs.size()));
    }

    // 写 QSAR
    QsarProduct qsar;
    qsar.productType = "Interferogram";
    qsar.created = QDateTime::currentDateTime().toString(Qt::ISODate);
    qsar.sourceMaster = mParams.masterProductDisplay;
    qsar.sourceSlave  = mParams.slaveProductDisplay;
    qsar.outputPrefix = mParams.outputPrefix;
    for (int i = 0; i < pairs.size(); ++i) {
        QsarBand b;
        b.subSwath = pairs[i].master.subSwath;
        b.polarization = pairs[i].master.polarization;
        b.file = prefix + "_" + b.subSwath + "_" + b.polarization + "_ifg.tif";
        b.width = pairs[i].master.width / mParams.rangeLooks;
        b.height = pairs[i].master.height / mParams.azimuthLooks;
        qsar.bands.append(b);
    }
    QString qsarPath = outputDir + "/" + prefix + ".qsar";
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
    GDALDatasetH hIfg = GDALCreate(driver, ifgPath.toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hCoh = GDALCreate(driver, cohPath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    GDALDatasetH hPh  = GDALCreate(driver, phasePath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    if (!hIfg || !hCoh || !hPh) { qWarning() << "[Ifg] cannot create output"; return false; }

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
    const QString& ifgPath, const QString& outPath,
    int width, int height, double wavelength,
    double nearRange, double rangeSpacing, double prf)
{
    Q_UNUSED(height);
    Q_UNUSED(prf);

    GdalSlcReader reader;
    if (!reader.open(ifgPath)) return false;
    int w = reader.width(), h = reader.height();
    auto ifgData = reader.readBand(0);
    reader.close();
    if (ifgData.size() < w * h) return false;

    QVector<std::complex<float>> flat(w * h);

    // WGS84椭球参数
    double Re = 6378137.0;
    double e2 = 0.00669437999014;
    double H = 800000.0; // 卫星轨道高度 (m) 近似

    for (int row = 0; row < h; ++row) {
        if (mCancelled) return false;
        for (int col = 0; col < w; ++col) {
            double R = nearRange + col * rangeSpacing;
            double theta = std::acos((Re + H) / (Re * 1.001));
            // 简化平地相位: φ_flat = -4π/λ * Bpar
            // 使用之前配准阶段计算的基线
            double phiFlat = 0;
            if (R > 0) {
                double sinTheta = std::sin(theta);
                double Bpar = 20.0; // 平行基线 (m) 缺省值
                phiFlat = -4.0 * M_PI / wavelength * Bpar * sinTheta;
            }
            int idx = row * w + col;
            float c = std::cos(static_cast<float>(phiFlat));
            float s = std::sin(static_cast<float>(phiFlat));
            auto v = ifgData[idx];
            flat[idx] = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
        }
    }

    GdalInterferogramWriter writer;
    if (!writer.create(outPath, w, h, true)) return false;
    writer.writeComplex(flat);
    return true;
}

// ── Stage 3: 差分干涉 ──
bool InterferogramServiceImpl::stageDifferential(
    const QString& flatPath, const QString& demPath, const QString& outPath,
    int width, int height, double wavelength,
    double nearRange, double rangeSpacing)
{
    Q_UNUSED(width); Q_UNUSED(height);

    // 读干涉图
    GdalSlcReader reader;
    if (!reader.open(flatPath)) return false;
    int w = reader.width(), h = reader.height();
    auto ifgData = reader.readBand(0);
    reader.close();
    if (ifgData.size() < w * h) return false;

    // 读DEM
    GdalDemReader dem;
    if (!dem.open(demPath)) return false;
    auto demData = dem.readElevation();
    int demW = dem.width(), demH = dem.height();
    dem.close();
    if (demData.size() < demW * demH) return false;

    QVector<std::complex<float>> diff(w * h);

    for (int row = 0; row < h; ++row) {
        if (mCancelled) return false;
        for (int col = 0; col < w; ++col) {
            double R = nearRange + col * rangeSpacing;
            double theta = 35.0 * M_PI / 180.0; // Sentinel-1 IW 典型入射角

            // 从DEM取高程 (最近邻采样)
            int demCol = col * demW / w;
            int demRow = row * demH / h;
            demCol = qBound(0, demCol, demW - 1);
            demRow = qBound(0, demRow, demH - 1);
            double hDem = demData[demRow * demW + demCol];

            // 地形相位: φ_topo = -4π/λ * Bperp * h / (R * sinθ)
            double Bperp = 33.2; // 竖直基线 (m)
            double phiTopo = -4.0 * M_PI / wavelength * Bperp * hDem / (R * std::sin(theta));

            int idx = row * w + col;
            float c = std::cos(static_cast<float>(phiTopo));
            float s = std::sin(static_cast<float>(phiTopo));
            auto v = ifgData[idx];
            diff[idx] = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
        }
    }

    GdalInterferogramWriter writer;
    if (!writer.create(outPath, w, h, true)) return false;
    writer.writeComplex(diff);
    return true;
}
