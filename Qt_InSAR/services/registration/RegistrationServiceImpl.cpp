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
#include "dataaccess/impl/GdalDemReader.h"
#include "dataaccess/impl/QsarIO.h"
#include "dataaccess/annotation/SlcAnnotation.h"
#include "domain/QsarProduct.h"

#include <gdal_priv.h>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QApplication>
#include <QFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <limits>

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
            slave->sensorInfo().acquisitionStart,
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

    // ── 地形校正配准: DEM 全量加载 (2026-08-16 相位质量根因修复) ──
    // 地形引起的距离偏移差 h·Δ(1/tanθ) ≈ h·0.0114 (~7 px @1500m) 未被
    // 平地多项式配准建模 → 山区逐像素失配 → 全分辨率去相关
    mTerrainDem.clear();
    mTerrainDemW = mTerrainDemH = 0;
    {
        QString demPath = mParams.demPath;
        if (qEnvironmentVariableIsSet("INSAR_TERRAIN_DEM"))
            demPath = qEnvironmentVariable("INSAR_TERRAIN_DEM");
        mTerrainSign = mParams.terrainOffsetSign;
        if (qEnvironmentVariableIsSet("INSAR_TERRAIN_SIGN"))
            mTerrainSign = qEnvironmentVariableIntValue("INSAR_TERRAIN_SIGN");
        if (!demPath.isEmpty()) {
            GdalDemReader dem;
            if (dem.open(demPath)) {
                const QVector<float> elev = dem.readElevation();
                if (!elev.isEmpty()) {
                    mTerrainDem.assign(elev.begin(), elev.end());
                    mTerrainDemW = dem.width();
                    mTerrainDemH = dem.height();
                    qDebug() << "[Reg] terrain-corrected coreg: DEM" << demPath
                             << mTerrainDemW << "x" << mTerrainDemH
                             << "sign=" << mTerrainSign
                             << "Bperp=" << std::abs(baseline.perpBaseline);
                }
                dem.close();
            } else {
                qWarning() << "[Reg] terrain DEM open failed:" << demPath;
            }
        }
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
    QVector<QualityReport> pairQuality;                 // 逐成功 pair 质量 (metadata.quality 汇总)
    QVector<QVector<BurstRegResult>> pairBurstResults;  // 逐成功 pair ESD 结果 (metadata.tops)
    QVector<int> okPairIdx;                             // 成功 pair 在 pairs 中的索引
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

        // 地形校正配准: 共享 DEM + 逐 pair 几何映射 (slave 斜距范围)
        if (!mTerrainDem.empty()) {
            ctx.demData = &mTerrainDem;
            ctx.demW = mTerrainDemW;
            ctx.demH = mTerrainDemH;
            const double sNear = pairs[i].s.nearRange > 0 ? pairs[i].s.nearRange
                : slave->sensorInfo().nearRange;
            const double sSpacing = pairs[i].s.rangeSpacing > 0 ? pairs[i].s.rangeSpacing
                : slave->sensorInfo().rangeSpacing;
            ctx.demNearRange = sNear;
            ctx.demFarRange = sNear + pairs[i].s.rasterSize.width() * sSpacing;
            ctx.demRangeSpacing = sSpacing;
            ctx.demBperp = std::abs(baseline.perpBaseline);
            ctx.demSign = mTerrainSign;
        }

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
            okPairIdx.append(i);
            pairQuality.append(ctx.qualityReport);
            pairBurstResults.append(ctx.burstResults);
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

        // ═══ Product Metadata (schema v2.0, 产品自描述) ═══
        ProductMetadata& meta = qsar.metadata;

        // pair 语义: 谁和谁配准、何时采集
        meta.pair.masterId = mParams.masterDisplayName;
        meta.pair.slaveId  = mParams.slaveDisplayName;
        meta.pair.masterTime = mSi.acquisitionStart.toString(Qt::ISODate);
        meta.pair.slaveTime  = slave->sensorInfo().acquisitionStart.toString(Qt::ISODate);

        // 轨道 (master/slave 各一份, 产品级; 下游基线/地理编码不再回找 XML)
        auto fillOrbit = [](QsarOrbitMeta& om, const QList<OrbitStateVector>& svs,
                            const QString& dir) {
            om.source = QStringLiteral("annotation");
            om.direction = dir;
            if (!svs.isEmpty())
                om.referenceTime = svs.first().utcTime.toString(Qt::ISODate);
            om.stateVectors = svs.toVector();
        };
        fillOrbit(meta.orbitMaster, master->orbitStateVectors(), mSi.orbitDirection);
        fillOrbit(meta.orbitSlave, slave->orbitStateVectors(),
                  slave->sensorInfo().orbitDirection);

        // baseline (pair 级, 由两轨道在此计算 — 单一事实源)
        if (baseline.valid) {
            meta.baseline.valid = true;
            meta.baseline.perpendicular = baseline.perpBaseline;
            meta.baseline.parallel = baseline.parBaseline;
            meta.baseline.temporal = baseline.temporalBaseline;
        }

        // 配准参数快照 (可追溯性)
        meta.processing.hasRegistration = true;
        meta.processing.registration.coarseMethod = lastCoarseMethod;
        meta.processing.registration.coarseWindowSize = mParams.coarseWindowSize;
        meta.processing.registration.coarseSearchWindow = mParams.coarseSearchWindow;
        meta.processing.registration.fineWindowSize = mParams.fineWindowSize;
        meta.processing.registration.polynomialDegree = mParams.polynomialDegree;
        meta.processing.registration.correlationThreshold = mParams.correlationThreshold;
        meta.processing.registration.resamplingMethod = mParams.resamplingMethod;
        meta.processing.registration.sincWindowSize = mParams.sincWindowSize;

        // 质量: qsar 摘要 + 详细报告外置
        if (!pairQuality.isEmpty()) {
            double sumCorr = 0, sumRmse = 0, sumValid = 0;
            for (const auto& q : pairQuality) {
                sumCorr += q.meanCorrelation;
                sumRmse += q.offsetRmse;
                sumValid += (q.totalPoints > 0)
                    ? static_cast<double>(q.validPoints) / q.totalPoints : 0.0;
            }
            meta.quality.meanCorrelation = sumCorr / pairQuality.size();
            meta.quality.offsetRmse = sumRmse / pairQuality.size();
            meta.quality.validRatio = sumValid / pairQuality.size();
            meta.quality.detailFile = QStringLiteral("registration_quality.json");
        }

        // TOPS 元数据 (canonical): 每子条带 burst 结构 + ESD 修正量
        for (int i = 0; i < pairs.size(); ++i) {
            QsarTopsSwath* sw = nullptr;
            for (auto& s : meta.tops.swaths)
                if (s.name == pairs[i].m.subSwath) { sw = &s; break; }
            if (!sw) {
                QsarTopsSwath ns;
                ns.name = pairs[i].m.subSwath;
                ns.burstCount = pairs[i].m.burstCount;
                ns.linesPerBurst = pairs[i].m.linesPerBurst;
                ns.azimuthFrequency = pairs[i].m.azimuthFrequency;
                for (int b = 0; b < pairs[i].m.burstStartLines.size(); ++b) {
                    QsarTopsBurst t;
                    t.index = b;
                    t.startLine = pairs[i].m.burstStartLines[b];
                    if (b < pairs[i].m.burstAzimuthTimes.size())
                        t.azimuthTime = pairs[i].m.burstAzimuthTimes[b].toString(Qt::ISODate);
                    ns.bursts.append(t);
                }
                meta.tops.swaths.append(ns);
                sw = &meta.tops.swaths.last();
            }
            // ESD 逐 burst 修正: 首个成功 pair 填充 (aziPoly 常数项 b0)
            for (int k = 0; k < okPairIdx.size(); ++k) {
                if (okPairIdx[k] != i) continue;
                for (const auto& br : pairBurstResults[k]) {
                    if (br.burstIndex >= 0 && br.burstIndex < sw->bursts.size()
                        && std::abs(sw->bursts[br.burstIndex].esdCorrection) < 1e-12) {
                        sw->bursts[br.burstIndex].esdCorrection = br.aziPoly.coeffs[0];
                    }
                }
                break;
            }
            // 辅影像多普勒质心多项式 (annotation dataDcPoly, 按 burst 方位时间最近匹配)
            // 注意: tops bursts 存的是主影像 burst 时间 (qsar 约定), 而 dcEstimate
            // 是辅影像的时间 — 两景相隔 12 天, 必须用辅影像自己的 burst 时间匹配!
            if (!sw->bursts.isEmpty() && sw->bursts[0].dcPoly.isEmpty()) {
                const QString key = sw->name + "/" + pairs[i].m.polarization;
                const SlcAnnotation san = slave->allAnnotations().value(key);
                const auto& sTimes = pairs[i].s.burstAzimuthTimes;
                if (!san.dopplerEstimates.isEmpty() && !sTimes.isEmpty()) {
                    for (int b = 0; b < sw->bursts.size() && b < sTimes.size(); ++b) {
                        if (!sTimes[b].isValid()) continue;
                        const DopplerEstimate* best = nullptr;
                        qint64 bestDt = std::numeric_limits<qint64>::max();
                        for (const auto& de : san.dopplerEstimates) {
                            if (!de.azimuthTime.isValid()) continue;
                            const qint64 dt = qAbs(de.azimuthTime.msecsTo(sTimes[b]));
                            if (dt < bestDt) { bestDt = dt; best = &de; }
                        }
                        if (best && bestDt < 5000 && !best->dataDcPoly.isEmpty()) {
                            sw->bursts[b].dcPoly = best->dataDcPoly;
                            sw->bursts[b].dcT0 = best->t0;
                        }
                    }
                }
            }
        }

        // 处理历史 (结构化)
        {
            QsarStageRecord rec;
            rec.name = QStringLiteral("registration");
            rec.time = QDateTime::currentDateTime().toString(Qt::ISODate);
            rec.softwareVersion = QsarIO::kSoftwareVersion;
            rec.params = QVariantMap{
                {QStringLiteral("level"), processingLevelName(mParams.level)},
                {QStringLiteral("coarseMethod"), lastCoarseMethod},
                {QStringLiteral("resamplingMethod"), mParams.resamplingMethod},
                {QStringLiteral("coarseWindowSize"), mParams.coarseWindowSize},
                {QStringLiteral("fineWindowSize"), mParams.fineWindowSize},
                {QStringLiteral("polynomialDegree"), mParams.polynomialDegree},
                {QStringLiteral("succeeded"), succeeded},
                {QStringLiteral("total"), static_cast<int>(pairs.size())}
            };
            qsar.history.append(rec);
        }
        qsar.stages << QStringLiteral("registration");

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
            // 详细质量报告 (逐 pair 残差/ESD 相位, qsar 只存摘要)
            if (!pairQuality.isEmpty()) {
                QJsonArray pairsArr;
                for (int k = 0; k < okPairIdx.size(); ++k) {
                    const int i = okPairIdx[k];
                    const QualityReport& q = pairQuality[k];
                    QJsonObject po;
                    po["subSwath"] = pairs[i].m.subSwath;
                    po["polarization"] = pairs[i].m.polarization;
                    po["meanCorrelation"] = q.meanCorrelation;
                    po["offsetRmse"] = q.offsetRmse;
                    po["rangeRmse"] = q.rangeRmse;
                    po["aziRmse"] = q.aziRmse;
                    po["polyRangeRmse"] = q.polyRangeRmse;
                    po["polyAziRmse"] = q.polyAziRmse;
                    po["esdMaxResidual"] = q.esdMaxResidual;
                    po["validRatio"] = (q.totalPoints > 0)
                        ? static_cast<double>(q.validPoints) / q.totalPoints : 0.0;
                    QJsonArray rmseArr;
                    for (double v : q.perBurstRmse) rmseArr.append(v);
                    po["perBurstRmse"] = rmseArr;
                    QJsonArray rRArr;
                    for (double v : q.perBurstRangeRmse) rRArr.append(v);
                    po["perBurstRangeRmse"] = rRArr;
                    QJsonArray rAArr;
                    for (double v : q.perBurstAziRmse) rAArr.append(v);
                    po["perBurstAziRmse"] = rAArr;
                    QJsonArray esdArr;
                    for (double v : q.esdPhaseDeltas) esdArr.append(v);
                    po["esdPhaseDeltas"] = esdArr;
                    pairsArr.append(po);
                }
                QJsonObject root;
                root["productType"] = QStringLiteral("RegistrationQuality");
                root["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
                root["pairs"] = pairsArr;
                QFile f(qsarDir + "/registration_quality.json");
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                    f.close();
                }
            }
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
