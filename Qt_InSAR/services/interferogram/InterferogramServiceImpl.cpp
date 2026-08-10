#include "InterferogramServiceImpl.h"
#include "PipelineContext.h"
#include "steps/IfgGenerator.h"
#include "steps/FlatEarthRemover.h"
#include "steps/TopoPhaseRemover.h"
#include "steps/IWMerger.h"
#include "dataaccess/impl/QsarIO.h"
#include "dataaccess/SarProductFactory.h"
#include "dataaccess/impl/GdalSlcReader.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMap>
#include <QPair>
#include <QScopedPointer>

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

    QsarProduct slaveQsar = QsarIO::read(mParams.slaveQsarPath);
    if (slaveQsar.bands.isEmpty()) {
        emit errorOccurred(QStringLiteral("辅影像QSAR无波段数据"));
        emit finished(false, QString()); mRunning = false; return;
    }

    QsarProduct masterQsar;
    QScopedPointer<ISarProduct> masterProduct;
    if (mParams.masterQsarPath.endsWith(".qsar", Qt::CaseInsensitive)) {
        masterQsar = QsarIO::read(mParams.masterQsarPath);
    } else {
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
            qb.burstCount = b.burstCount;
            qb.linesPerBurst = b.linesPerBurst;
            qb.burstStartLines = b.burstStartLines;
            qb.burstAzimuthTimes = b.burstAzimuthTimes;
            qb.azimuthFrequency = b.azimuthFrequency;
            masterQsar.bands.append(qb);
        }
    }

    if (masterQsar.bands.isEmpty()) {
        emit errorOccurred(QStringLiteral("主影像无波段数据"));
        emit finished(false, QString()); mRunning = false; return;
    }

    // 按 subSwath + polarization 配对
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

    int succeeded = 0;
    for (int i = 0; i < pairs.size(); ++i) {
        if (mCancelled) break;

        QString sw = pairs[i].master.subSwath;
        QString pol = pairs[i].master.polarization;
        QString pairName = QStringLiteral("%1_%2").arg(sw).arg(pol);
        int basePct = i * 100 / pairs.size();
        emit progressChanged(basePct, QStringLiteral("处理 %1/%2: %3").arg(i+1).arg(pairs.size()).arg(pairName));

        // 构建 PipelineContext
        IfgPipelineContext ctx;
        ctx.params     = &mParams;
        ctx.masterPath = pairs[i].master.file;
        ctx.slavePath  = pairs[i].slave.file;
        // 提取 master ZIP 路径 + entry (供 SentinelDataReader 使用)
        {
            QString p = pairs[i].master.file;
            if (p.startsWith("/vsizip/")) {
                QString inner = p.mid(8);
                int zipEnd = inner.indexOf(".zip/", 0, Qt::CaseInsensitive);
                if (zipEnd >= 0) {
                    ctx.masterZip   = inner.left(zipEnd + 4);
                    ctx.masterEntry = inner.mid(zipEnd + 5);
                }
            }
        }
        ctx.width      = pairs[i].master.width;
        ctx.height     = pairs[i].master.height;
        ctx.burstInfo  = &pairs[i].slave;
        ctx.ifgOutputBase = ifgDir + "/" + pairName;
        ctx.outputBand.subSwath = sw;
        ctx.outputBand.polarization = pol;
        ctx.outputBand.width  = mParams.rangeLooks > 0 ? pairs[i].master.width / mParams.rangeLooks : pairs[i].master.width;
        ctx.outputBand.height = mParams.azimuthLooks > 0 ? pairs[i].master.height / mParams.azimuthLooks : pairs[i].master.height;

        // ── 构建步骤链 ──
        QVector<IIfgStep*> steps;
        steps << new IfgGenerator;
        if (mParams.enableFlatEarth)
            steps << new FlatEarthRemover;
        if (mParams.enableDifferential && !mParams.demPath.isEmpty())
            steps << new TopoPhaseRemover;

        bool pairOk = true;
        for (int si = 0; si < steps.size(); ++si) {
            if (mCancelled) { pairOk = false; break; }
            IIfgStep* step = steps[si];
            int stepPct = basePct + (si + 1) * (100 / pairs.size()) / (steps.size() + 1);
            emit progressChanged(stepPct, pairName + QStringLiteral(": %1").arg(step->name()));

            if (!step->execute(ctx)) {
                qWarning() << "[Ifg]" << step->name() << "failed:" << pairName
                           << "-" << ctx.errorMessage;
                pairOk = false;
                break;
            }
        }

        qDeleteAll(steps);

        if (!pairOk) continue;

        // 写入 QSAR 波段信息
        QsarBand qb = ctx.outputBand;
        qb.file = QStringLiteral("ifg/%1_ifg.tif").arg(pairName);
        qb.ifgFile  = qb.file;
        qb.cohFile  = QStringLiteral("ifg/%1_coh.tif").arg(pairName);
        qb.phaseFile = QStringLiteral("ifg/%1_phase.tif").arg(pairName);
        qb.layerType = "ifg";
        qb.defaultVisible = false;  // 复数干涉图不自动加载

        // 更新实际输出尺寸
        {
            GdalSlcReader dimRdr;
            if (dimRdr.open(ifgDir + "/" + pairName + "_ifg.tif")) {
                qb.width = dimRdr.width();
                qb.height = dimRdr.height();
                dimRdr.close();
            }
        }

        if (mParams.enableFlatEarth && !qb.flatFile.isEmpty() && !qsar.stages.contains("flat"))
            qsar.stages << "flat";
        if (mParams.enableDifferential && !qb.diffFile.isEmpty() && !qsar.stages.contains("diff"))
            qsar.stages << "diff";

        qsar.bands.append(qb);
        // 相位图层 (可见)
        QsarBand qbPhase;
        qbPhase.subSwath = sw; qbPhase.polarization = pol;
        qbPhase.file = QStringLiteral("ifg/%1_phase.tif").arg(pairName);
        qbPhase.phaseFile = qbPhase.file;
        qbPhase.layerType = "phase";
        qbPhase.defaultVisible = true;
        qsar.bands.append(qbPhase);
        // 相干图层 (可见)
        QsarBand qbCoh;
        qbCoh.subSwath = sw; qbCoh.polarization = pol;
        qbCoh.file = QStringLiteral("ifg/%1_coh.tif").arg(pairName);
        qbCoh.cohFile = qbCoh.file;
        qbCoh.layerType = "coherence";
        qbCoh.defaultVisible = true;
        qsar.bands.append(qbCoh);
        ++succeeded;
        emit progressChanged((i + 1) * 100 / pairs.size(), QStringLiteral("完成 %1/%2").arg(i+1).arg(pairs.size()));
    }

    // ═══ IW Merge: 同极化 IW1+IW2+IW3 拼接为宽幅产品 ═══
    if (succeeded > 0) {
        QDir().mkpath(outputDir + "/merge");
        // 收集 per-IW 输出路径和尺寸, 按极化分组
        struct IwOut { QString swath; int w, h; };
        QMap<QString, QVector<QPair<QString, IwOut>>> byPol;  // pol → [(pairName, IwOut)]
        for (int i = 0; i < pairs.size(); ++i) {
            QString sw = pairs[i].master.subSwath;
            QString pol = pairs[i].master.polarization;
            QString name = QStringLiteral("%1_%2").arg(sw).arg(pol);
            // 从已生成的 ifg 文件获取尺寸
            QString ifgPath = ifgDir + "/" + name + "_ifg.tif";
            GdalSlcReader dimRdr;
            IwOut io; io.swath = sw;
            if (dimRdr.open(ifgPath)) { io.w = dimRdr.width(); io.h = dimRdr.height(); dimRdr.close(); }
            else continue;
            byPol[pol].append({name, io});
        }
        for (auto it = byPol.begin(); it != byPol.end(); ++it) {
            QString pol = it.key();
            auto& iwList = it.value();
            if (iwList.size() < 2) continue;  // 至少 2 个 IW 才需要 merge
            // 按 IW1→IW2→IW3 排序
            std::sort(iwList.begin(), iwList.end(),
                [](const auto& a, const auto& b) { return a.second.swath < b.second.swath; });

            QVector<QString> phaseFiles, cohFiles, ifgFiles;
            QVector<IWMerger::IwMeta> metas;
            for (auto& iw : iwList) {
                QString base = ifgDir + "/" + iw.first;
                phaseFiles.append(base + "_phase.tif");
                cohFiles.append(base + "_coh.tif");
                ifgFiles.append(base + "_ifg.tif");
                IWMerger::IwMeta m;
                m.swath = iw.second.swath;
                m.width = iw.second.w;
                m.height = iw.second.h;
                // 从 master 产品获取 per-swath range 参数
                m.nearRange = 0; m.rangeSpacing = 2.3;  // fallback
                if (masterProduct) {
                    for (const auto& b : masterProduct->bands()) {
                        if (b.subSwath == iw.second.swath) {
                            m.nearRange = b.nearRange;
                            m.rangeSpacing = b.rangeSpacing > 0 ? b.rangeSpacing : 2.3;
                            break;
                        }
                    }
                }
                metas.append(m);
            }
            QString mergeBase = outputDir + "/merge/S1_" + pol;
            emit progressChanged(95, QStringLiteral("IW Merge %1...").arg(pol));
            IWMerger::mergePhase(phaseFiles, metas, mergeBase + "_phase.tif");
            IWMerger::mergeCoherence(cohFiles, metas, mergeBase + "_coh.tif");
            IWMerger::mergeComplex(ifgFiles, metas, mergeBase + "_ifg.tif");
            // 新增 merge 图层到 qsar
            QsarBand qbM;
            qbM.subSwath = "IW"; qbM.polarization = pol;
            qbM.file = QStringLiteral("merge/S1_%1_phase.tif").arg(pol);
            qbM.layerType = "phase";
            qbM.defaultVisible = true;
            qsar.bands.append(qbM);
        }
    }

    QString qsarPath = outputDir + "/" + mParams.outputPrefix + ".qsar";
    QsarIO::write(qsarPath, qsar);

    emit progressChanged(100, QStringLiteral("干涉图生成完成 (%1/%2对)").arg(succeeded).arg(pairs.size()));
    emit finished(succeeded > 0, qsarPath);
    mRunning = false;
}
