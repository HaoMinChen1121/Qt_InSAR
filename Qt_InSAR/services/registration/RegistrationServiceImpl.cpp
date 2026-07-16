#include "RegistrationServiceImpl.h"
#include "PipelineContext.h"

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
        QVector<IRegStep*> steps;
        steps << new DataReader
              << new BurstMatcher
              << new OrbitInitializer
              << new CoarseCorrelator
              << new OffsetExtractor
              << new PolynomialFitter
              << new FineCorrelator
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
        if (ctx.masterReader) { ctx.masterReader->close(); delete ctx.masterReader; }
        if (ctx.slaveReader)  { ctx.slaveReader->close();  delete ctx.slaveReader; }
        qDeleteAll(steps);

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
            b.subSwath = pairs[i].m.subSwath; b.polarization = pairs[i].m.polarization;
            b.width = pairs[i].m.rasterSize.width(); b.height = pairs[i].m.rasterSize.height();
            QString pn = QStringLiteral("%1of%2_%3_%4").arg(i+1).arg(pairs.size()).arg(b.subSwath).arg(b.polarization);
            QString op = mParams.outputDir.isEmpty()
                ? QDir::tempPath() + "/" + prefix + "_" + pn + "_reg.tif"
                : mParams.outputDir + "/" + prefix + "_" + pn + "_reg.tif";
            b.file = QFileInfo(op).fileName(); qsar.bands.append(b);
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
