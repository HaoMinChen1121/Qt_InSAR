#include "RegistrationServiceImpl.h"
#include "PipelineContext.h"
#include "strategy/StrategyFactory.h"
#include "strategy/ProductDetector.h"
#include "strategy/PipelineBuilder.h"
#include "steps/IRegStep.h"
#include "dataaccess/impl/SentinelDataReader.h"

#include "dataaccess/SarProductFactory.h"
#include "algorithms/BaselineEstimator.h"
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

    // 从轨道向量计算基线
    SarSensorInfo mSi = master->sensorInfo();
    sar::BaselineResult baseline;
    if (mParams.estimateBaseline) {
        baseline = sar::computeBaseline(
            master->orbitStateVectors(),
            slave->orbitStateVectors(),
            mSi.acquisitionStart,
            mSi.nearRange,
            mSi.farRange);
        if (baseline.valid) {
            mParams.baselinePerp = baseline.perpBaseline;
            mParams.baselinePar  = baseline.parBaseline;
        }
    }
    if (baseline.valid) {
        qDebug() << QStringLiteral("[Reg] pair: master=%1 slave=%2 baselinePerp=%3m baselinePar=%4m")
            .arg(mParams.masterDisplayName, mParams.slaveDisplayName)
            .arg(baseline.perpBaseline, 0, 'f', 1)
            .arg(baseline.parBaseline, 0, 'f', 1);
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
    QString lastCoarseMethod = QStringLiteral("FFT");
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

        // ── 策略解析链: ProductDetector → ProductMode + Level →
        //    StrategyFactory → 默认策略 → applyOverrides → 最终策略 ──
        QString detectWarning;
        ProductMode pm = ProductDetector::detect(master->sensorInfo(),
                                                 pairs[i].m.burstCount, &detectWarning);
        RegistrationStrategy strat = StrategyFactory::create(pm, mParams.level);
        strat.applyOverrides(mParams);
        if (!detectWarning.isEmpty())
            qWarning() << "[Reg] ProductDetector:" << detectWarning;
        qDebug() << "[Reg] strategy:" << productModeName(pm)
                 << "level=" << processingLevelName(mParams.level)
                 << "coarse=" << correlationMethodName(strat.coarseCorr)
                 << "fine=" << strat.useFine
                 << "esd=" << (strat.azimuthCorrection == AzimuthCorrection::ESD);
        lastCoarseMethod = correlationMethodName(strat.coarseCorr);

        PipelineContext ctx;
        ctx.params     = &mParams;
        ctx.strategy   = &strat;
        ctx.masterBand = &pairs[i].m;
        ctx.slaveBand  = &pairs[i].s;
        ctx.masterSensorInfo = master->sensorInfo();
        ctx.pairIndex  = i;
        ctx.totalPairs = pairs.size();
        ctx.outputPath = outPath;

        // 运行管道 (步骤链由策略构建; 阶段4: 所有策略返回相同的 11 步)
        QVector<IRegStep*> steps = buildPipelineSteps(strat);

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
        qsar.coarseMethod = lastCoarseMethod;
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
