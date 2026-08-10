#include "RegistrationServiceImpl.h"
#include "PipelineContext.h"
#include "dataaccess/impl/SentinelDataReader.h"

#include "steps/DataReader.h"
#include "steps/BurstMatcher.h"
#include "steps/OrbitInitializer.h"
#include "steps/CoarseCorrelator.h"
#include "steps/OffsetExtractor.h"
#include "steps/PolynomialFitter.h"
#include "steps/FineCorrelator.h"
#include "steps/EsdCorrector.h"
#include "steps/SincResampler.h"
#include "steps/QualityEvaluator.h"

#include "dataaccess/SarProductFactory.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/QsarIO.h"
#include "domain/QsarProduct.h"

#include <gdal_priv.h>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QApplication>

RegistrationServiceImpl::RegistrationServiceImpl(QObject* parent)
    : IRegistrationService(parent) {}

void RegistrationServiceImpl::setParams(const RegistrationParams& p) { mParams = p; }
RegistrationParams RegistrationServiceImpl::params() const { return mParams; }
bool RegistrationServiceImpl::isRunning() const { return mRunning; }
void RegistrationServiceImpl::cancel() { mCancelled = true; }

void RegistrationServiceImpl::execute() {
    mRunning = true; mCancelled = false;
    GDALAllRegister();

    QString mProd = mParams.masterProductPath.isEmpty() ? mParams.masterPath : mParams.masterProductPath;
    QString sProd = mParams.slaveProductPath.isEmpty() ? mParams.slavePath : mParams.slaveProductPath;

    QScopedPointer<ISarProduct> master(createSarProduct(mProd));
    QScopedPointer<ISarProduct> slave(createSarProduct(sProd));
    if (!master || !master->open(mProd)) {
        emit errorOccurred(QStringLiteral("无法打开主产品: %1").arg(mProd));
        emit finished(false, {}); mRunning = false; return;
    }
    if (!slave || !slave->open(sProd)) {
        emit errorOccurred(QStringLiteral("无法打开辅产品: %1").arg(sProd));
        emit finished(false, {}); mRunning = false; return;
    }

    const auto& mBands = master->bands();
    const auto& sBands = slave->bands();

    struct BandPair { SarBandDescriptor m, s; };
    QVector<BandPair> pairs;
    for (const auto& mb : mBands)
        for (const auto& sb : sBands)
            if (mb.subSwath == sb.subSwath && mb.polarization == sb.polarization)
                { pairs.append({mb, sb}); break; }

    if (pairs.isEmpty()) {
        emit errorOccurred(QStringLiteral("未找到可配对的波段"));
        emit finished(false, {}); mRunning = false; return;
    }

    QString slaveDate;
    if (!mParams.slaveDisplayName.isEmpty()) {
        auto dp = mParams.slaveDisplayName.split('_');
        if (dp.size() >= 2) slaveDate = dp[1];
    }
    QString prefix = slaveDate.isEmpty() ? mParams.outputPrefix : slaveDate + "_" + mParams.outputPrefix;

    int succeeded = 0; QString lastOut;
    for (int i = 0; i < pairs.size(); ++i) {
        if (mCancelled) break;
        emit progressChanged(i * 100 / pairs.size(),
            QStringLiteral("配准 %1/%2: %3 %4").arg(i+1).arg(pairs.size())
                .arg(pairs[i].m.subSwath).arg(pairs[i].m.polarization));

        QString pairName = QStringLiteral("%1of%2_%3_%4")
            .arg(i+1).arg(pairs.size()).arg(pairs[i].m.subSwath).arg(pairs[i].m.polarization);
        QString outPath = mParams.outputDir.isEmpty()
            ? QDir::tempPath() + "/" + prefix + "_" + pairName + "_reg.tif"
            : mParams.outputDir + "/" + prefix + "_" + pairName + "_reg.tif";

        PipelineContext ctx;
        ctx.params     = &mParams;
        ctx.masterBand = &pairs[i].m;
        ctx.slaveBand  = &pairs[i].s;
        ctx.pairIndex  = i;
        ctx.totalPairs = pairs.size();
        ctx.outputPath = outPath;

        // 运行 10 步管道
        // FineCorrelator 必须在 PolynomialFitter 之前 — 多项式需要在精化后的点上拟合
        QVector<IRegStep*> steps;
        steps << new DataReader
              << new BurstMatcher
              << new OrbitInitializer
              << new CoarseCorrelator
              << new OffsetExtractor
              << new FineCorrelator
              << new PolynomialFitter
              << new EsdCorrector
              << new SincResampler
              << new QualityEvaluator;

        bool ok = true;
        for (int si = 0; si < steps.size(); ++si) {
            if (mCancelled) { ok = false; break; }
            emit progressChanged(i * 100 / pairs.size() + si,
                QStringLiteral("[%1/%2] %3").arg(i+1).arg(pairs.size()).arg(steps[si]->name()));
            if (!steps[si]->execute(ctx)) {
                qWarning() << "[Reg] step failed:" << steps[si]->name() << ctx.errorMessage;
                ok = false; break;
            }
        }

        // 清理 reader
        if (ctx.masterReader) {
            ctx.masterReader->close();
            delete ctx.masterReader;
            ctx.masterReader = nullptr;
        }
        if (ctx.slaveReader) {
            ctx.slaveReader->close();
            delete ctx.slaveReader;
            ctx.slaveReader = nullptr;
        }
        // 清理 SentinelDataReader
        if (ctx.masterSdr) {
            ctx.masterSdr->close();
            delete ctx.masterSdr;
            ctx.masterSdr = nullptr;
        }
        if (ctx.slaveSdr) {
            ctx.slaveSdr->close();
            delete ctx.slaveSdr;
            ctx.slaveSdr = nullptr;
        }
        qDeleteAll(steps);
        steps.clear();

        qDebug() << QStringLiteral("[Reg] pair %1/%2 done").arg(i+1).arg(pairs.size());

        if (ok) {
            ++succeeded; lastOut = outPath;
            qDebug() << QStringLiteral("[Reg] %1 OK rmse=%2 corr=%3")
                .arg(pairName).arg(ctx.qualityReport.offsetRmse, 0, 'f', 4)
                .arg(ctx.qualityReport.meanCorrelation, 0, 'f', 4);
        }
        QApplication::processEvents();
    }

    // QSAR 输出
    if (succeeded > 0) {
        QsarProduct qsar;
        qsar.productType = "RegisteredSLC";
        qsar.created = QDateTime::currentDateTime().toString(Qt::ISODate);
        qsar.sourceMaster = mParams.masterDisplayName;
        qsar.sourceSlave  = mParams.slaveDisplayName;
        qsar.coarseMethod = "FFT";
        qsar.resamplingMethod = mParams.resamplingMethod;
        qsar.outputPrefix = mParams.outputPrefix;
        QString qsarDir;
        for (int i = 0; i < pairs.size(); ++i) {
            QsarBand b;
            QString sw = pairs[i].m.subSwath, pol = pairs[i].m.polarization;
            b.subSwath = sw; b.polarization = pol;
            // 输出路径
            QString pn = QStringLiteral("%1of%2_%3_%4").arg(i+1).arg(pairs.size()).arg(sw).arg(pol);
            QString op = mParams.outputDir.isEmpty()
                ? QDir::tempPath() + "/" + prefix + "_" + pn + "_reg.tif"
                : mParams.outputDir + "/" + prefix + "_" + pn + "_reg.tif";
            // rasterSize 在 Sentinel1Product 中未填充, 从输出文件获取真实尺寸
            { GdalSlcReader rd; if (rd.open(op)) { b.width = rd.width(); b.height = rd.height(); rd.close(); } }
            b.file = QFileInfo(op).fileName();
            b.layerType = "amplitude";
            b.defaultVisible = true;  // 注册结果自动加载到画布
            // 写入 burst 元数据 → IfgGenerator 的 TOPSAR deburst 路径需要
            b.burstCount        = pairs[i].m.burstCount;
            b.linesPerBurst     = pairs[i].m.linesPerBurst;
            b.burstStartLines   = pairs[i].m.burstStartLines;
            b.burstAzimuthTimes = pairs[i].m.burstAzimuthTimes;
            b.azimuthFrequency  = pairs[i].m.azimuthFrequency;
            qsar.bands.append(b);
            qsarDir = QFileInfo(op).absolutePath();
        }
        if (!qsarDir.isEmpty()) {
            QsarIO::write(qsarDir + "/" + prefix + ".qsar", qsar);
            lastOut = qsarDir + "/" + prefix + ".qsar";
        }
        emit progressChanged(100, QStringLiteral("配准完成 (%1/%2对)").arg(succeeded).arg(pairs.size()));
        emit finished(true, lastOut);
    } else {
        emit errorOccurred(QStringLiteral("所有波段对配准失败"));
        emit finished(false, {});
    }
    mRunning = false;
}
