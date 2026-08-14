#include "InterferogramServiceImpl.h"
#include "PipelineContext.h"
#include "steps/IfgGenerator.h"
#include "steps/TopsarDeburst.h"
#include "steps/FlatEarthRemover.h"
#include "steps/TopoPhaseRemover.h"
#include "steps/IWMerger.h"
#include "steps/GeomTable.h"
#include "steps/PhaseVisualizer.h"
#include "dataaccess/impl/QsarIO.h"
#include "dataaccess/SarProductFactory.h"
#include "algorithms/BaselineEstimator.h"
#include "dataaccess/impl/GdalSlcReader.h"

#include <gdal_priv.h>

#include <QDebug>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QMap>
#include <QPair>
#include <QScopedPointer>
#include <cmath>
#include <limits>
#include <vector>

namespace {

// 从合并产品按列切片生成逐 IW 兼容输出 (legacyPerIwOutputs, 设计 §10)
bool sliceColumns(const QString& srcPath, const QString& dstPath,
                  int col0, int nCols)
{
    if (srcPath.isEmpty() || nCols <= 0) return false;
    GDALDatasetH src = GDALOpen(srcPath.toUtf8().constData(), GA_ReadOnly);
    if (!src) return false;
    GDALRasterBandH sb = GDALGetRasterBand(src, 1);
    const int h = GDALGetRasterYSize(src);
    const GDALDataType dt = GDALGetRasterDataType(sb);

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH dst = GDALCreate(drv, dstPath.toUtf8().constData(), nCols, h, 1, dt, nullptr);
    if (!dst) { GDALClose(src); return false; }
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALSetGeoTransform(dst, gt);

    const int pxlSize = GDALGetDataTypeSize(dt) / 8;
    std::vector<char> buf(static_cast<size_t>(nCols) * pxlSize);
    for (int r = 0; r < h; ++r) {
        GDALRasterIO(sb, GF_Read, col0, r, nCols, 1, buf.data(), nCols, 1, dt, 0, 0);
        GDALRasterIO(GDALGetRasterBand(dst, 1), GF_Write, 0, r, nCols, 1, buf.data(), nCols, 1, dt, 0, 0);
    }
    GDALClose(dst);
    GDALClose(src);
    qDebug() << "[Ifg] legacy slice" << col0 << "+" << nCols << "->" << dstPath;
    return true;
}

} // namespace

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
        // 注: 准确基线需辅影像轨道向量 (辅为 .qsar 时无轨道), 由调用方通过 params 传入
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
    QString deburstDir = outputDir + "/deburst";
    QString flatDir = outputDir + "/flat";
    QString diffDir = outputDir + "/diff";
    QDir().mkpath(deburstDir);
    QDir().mkpath(deburstDir + "/.tmp");   // 临时 burst 块 (deburst 后删除)
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
        if (masterProduct)
            ctx.masterSensorInfo = masterProduct->sensorInfo();
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
        ctx.masterBurstInfo = &pairs[i].master;   // deburst 方位校正 t_ref 来源
        ctx.deburstOutputBase = deburstDir + "/" + pairName;
        ctx.burstBlockBase    = deburstDir + "/.tmp/" + pairName;
        ctx.azimuthRampCorrectionSign = mParams.azimuthRampCorrectionSign;

        // master k_t (方位调频率): 逐 band annotation 值, 缺失时轨道估算
        ctx.azimuthFmRate = 0.0;
        if (masterProduct) {
            for (const auto& b : masterProduct->bands()) {
                if (b.subSwath == sw && b.polarization == pol) {
                    ctx.azimuthFmRate = b.azimuthFmRate;
                    if (std::abs(ctx.azimuthFmRate) < 1e-9) {
                        // 回退: kt ≈ −2·Vr²/(λ·R0)
                        const auto& ov = masterProduct->orbitStateVectors();
                        double v2 = 0; int nv = 0;
                        for (const auto& o : ov) {
                            v2 += o.vx * o.vx + o.vy * o.vy + o.vz * o.vz;
                            ++nv;
                        }
                        if (nv > 0) {
                            v2 /= nv;
                            double lambda = mParams.wavelength > 0
                                ? mParams.wavelength : 0.05546576;
                            double r0 = b.nearRange
                                + b.rasterSize.width() / 2.0 * b.rangeSpacing;
                            if (lambda > 0 && r0 > 0)
                                ctx.azimuthFmRate = -2.0 * v2 / (lambda * r0);
                        }
                    }
                    break;
                }
            }
        }
        if (std::abs(ctx.azimuthFmRate) < 1e-9)
            qWarning() << "[Ifg] master k_t unavailable for" << pairName
                       << "(方位校正将跳过; master 为 .qsar 时属预期)";

        ctx.outputBand.subSwath = sw;
        ctx.outputBand.polarization = pol;
        ctx.outputBand.width  = mParams.rangeLooks > 0 ? pairs[i].master.width / mParams.rangeLooks : pairs[i].master.width;
        ctx.outputBand.height = mParams.azimuthLooks > 0 ? pairs[i].master.height / mParams.azimuthLooks : pairs[i].master.height;

        // ── 构建步骤链 (Stage 2: 逐对只做干涉+deburst; flat/topo 移到合并产品) ──
        QVector<IIfgStep*> steps;
        steps << new IfgGenerator;
        if (pairs[i].slave.burstCount > 1)
            steps << new TopsarDeburst;

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
        qb.file = QStringLiteral("deburst/%1_ifg.tif").arg(pairName);
        qb.ifgFile  = qb.file;
        qb.cohFile  = QStringLiteral("deburst/%1_coh.tif").arg(pairName);
        qb.phaseFile = QStringLiteral("deburst/%1_phase.tif").arg(pairName);
        qb.layerType = "ifg";
        qb.defaultVisible = false;  // 复数干涉图不自动加载

        // 更新实际输出尺寸
        {
            GdalSlcReader dimRdr;
            if (dimRdr.open(deburstDir + "/" + pairName + "_ifg.tif")) {
                qb.width = dimRdr.width();
                qb.height = dimRdr.height();
                dimRdr.close();
            }
        }

        qsar.bands.append(qb);
        // 相位图层 (中间体, 默认不加载 — 可见产品为合并后的 merge 图层)
        QsarBand qbPhase;
        qbPhase.subSwath = sw; qbPhase.polarization = pol;
        qbPhase.file = QStringLiteral("deburst/%1_phase.tif").arg(pairName);
        qbPhase.phaseFile = qbPhase.file;
        qbPhase.layerType = "phase";
        qbPhase.defaultVisible = false;
        qsar.bands.append(qbPhase);
        // 相干图层 (中间体, 默认不加载)
        QsarBand qbCoh;
        qbCoh.subSwath = sw; qbCoh.polarization = pol;
        qbCoh.file = QStringLiteral("deburst/%1_coh.tif").arg(pairName);
        qbCoh.cohFile = qbCoh.file;
        qbCoh.layerType = "coherence";
        qbCoh.defaultVisible = false;
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
            QString ifgPath = deburstDir + "/" + name + "_ifg.tif";
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
            QDateTime tRef;   // 首个子条带的首 burst 方位时间 (方位对齐参考)
            for (auto& iw : iwList) {
                QString base = deburstDir + "/" + iw.first;
                phaseFiles.append(base + "_phase.tif");
                cohFiles.append(base + "_coh.tif");
                ifgFiles.append(base + "_ifg.tif");
                IWMerger::IwMeta m;
                m.swath = iw.second.swath;
                m.width = iw.second.w;
                m.height = iw.second.h;
                m.rangeLooks = mParams.rangeLooks;
                // 从 master 产品获取 per-swath range 参数 + 方位时间
                m.nearRange = 0; m.rangeSpacing = 0.0;
                if (masterProduct) {
                    for (const auto& b : masterProduct->bands()) {
                        if (b.subSwath == iw.second.swath) {
                            m.nearRange = b.nearRange;
                            m.rangeSpacing = b.rangeSpacing > 0 ? b.rangeSpacing : 0.0;
                            m.fullResWidth = b.rasterSize.width();
                            // 子条带间方位对齐: 用首 burst 方位时间差
                            if (!b.burstAzimuthTimes.isEmpty()) {
                                if (tRef.isNull())
                                    tRef = b.burstAzimuthTimes.first();
                                double prf = b.azimuthFrequency > 0
                                    ? b.azimuthFrequency : mParams.prf;
                                double dt = tRef.msecsTo(
                                    b.burstAzimuthTimes.first()) / 1000.0;
                                int azLooks = mParams.azimuthLooks > 0
                                    ? mParams.azimuthLooks : 1;
                                m.azimuthOffset = static_cast<int>(
                                    std::lround(dt * prf / azLooks));
                            }
                            break;
                        }
                    }
                }
                metas.append(m);
            }
            // 偏移归一化 ≥ 0
            {
                int minOff = std::numeric_limits<int>::max();
                for (const auto& m : metas)
                    minOff = std::min(minOff, m.azimuthOffset);
                for (auto& m : metas)
                    m.azimuthOffset -= minOff;
            }
            QString mergeBase = outputDir + "/merge/S1_" + pol;
            emit progressChanged(95, QStringLiteral("IW Merge %1...").arg(pol));
            IWMerger::mergePhase(phaseFiles, cohFiles, metas, mergeBase + "_phase.tif",
                                 mParams.phaseAlign);
            IWMerger::mergeCoherence(cohFiles, metas, mergeBase + "_coh.tif");
            IWMerger::mergeComplex(ifgFiles, cohFiles, phaseFiles, metas,
                                   mergeBase + "_ifg.tif", mParams.phaseAlign);

            // ── Stage 2: 几何表 (逐列 R/θ) + 合并产品上去平地/差分 ──
            GeomTable geomTable;
            {
                GdalSlcReader dimRdr;
                if (dimRdr.open(mergeBase + "_ifg.tif")) {
                    geomTable.width = dimRdr.width();
                    dimRdr.close();
                }
            }
            {
                int trim = 0;   // 与 IWMerger 相同的重叠裁剪 (输出列)
                for (int i = 0; i < metas.size() - 1; ++i) {
                    int fullW = metas[i].fullResWidth > 0
                        ? metas[i].fullResWidth
                        : metas[i].width * std::max(1, metas[i].rangeLooks);
                    double leftFar = metas[i].nearRange + fullW * metas[i].rangeSpacing;
                    double rightNear = metas[i + 1].nearRange;
                    double overlap = leftFar - rightNear;
                    if (overlap > 0) {
                        double avgSp = (metas[i].rangeSpacing + metas[i + 1].rangeSpacing) / 2.0;
                        if (avgSp > 0) {
                            int fullRes = static_cast<int>(overlap / avgSp + 0.5);
                            int rg = std::max(1, metas[i].rangeLooks);
                            trim = std::max(1, (fullRes + rg / 2) / rg);
                        }
                    }
                    if (trim <= 0) trim = 50;
                }
                int cumCol = 0;
                for (int i = 0; i < metas.size(); ++i) {
                    SwathGeom g;
                    g.name = metas[i].swath;
                    g.startCol = cumCol;
                    // 与 IWMerger 一致: 重叠区两侧都裁剪 (滚降带移除)
                    const int trimLeft = (i > 0 ? trim : 0);
                    const int trimRight = (i < metas.size() - 1 ? trim : 0);
                    g.width = metas[i].width - trimLeft - trimRight;
                    g.nearRange = metas[i].nearRange
                        + trimLeft * metas[i].rangeSpacing
                          * std::max(1, metas[i].rangeLooks);
                    g.rangeSpacing = metas[i].rangeSpacing
                        * std::max(1, metas[i].rangeLooks);   // 输出列间距
                    cumCol += g.width;
                    geomTable.swaths.append(g);
                }
            }
            geomTable.save(mergeBase + "_geom.json");

            IfgPipelineContext fctx;
            fctx.params = &mParams;
            if (masterProduct)
                fctx.masterSensorInfo = masterProduct->sensorInfo();
            fctx.mergeOutputBase = mergeBase;
            fctx.geomTablePath   = mergeBase + "_geom.json";
            fctx.flatOutputBase  = flatDir + "/S1_" + pol;
            fctx.diffOutputBase  = diffDir + "/S1_" + pol;
            fctx.visualizationOutputBase = outputDir + "/visualization/S1_" + pol;
            fctx.outputBand.subSwath = "IW";
            fctx.outputBand.polarization = pol;

            bool mergedOk = true;
            if (mParams.enableFlatEarth) {
                FlatEarthRemover flatStep;
                if (!flatStep.execute(fctx)) {
                    qWarning() << "[Ifg] FlatEarth (merged) failed:" << fctx.errorMessage;
                    mergedOk = false;
                } else if (!qsar.stages.contains("flat")) {
                    qsar.stages << "flat";
                }
            }
            if (mergedOk && mParams.enableDifferential && !mParams.demPath.isEmpty()) {
                TopoPhaseRemover topoStep;
                if (!topoStep.execute(fctx)) {
                    qWarning() << "[Ifg] TopoPhase (merged) failed:" << fctx.errorMessage;
                    mergedOk = false;
                } else if (!qsar.stages.contains("diff")) {
                    qsar.stages << "diff";
                }
            }

            // Stage 3: HSV 彩色渲染
            bool visOk = false;
            if (mergedOk && mParams.enableVisualization) {
                PhaseVisualizer vis;
                visOk = vis.execute(fctx);
                if (visOk && !qsar.stages.contains("visualization"))
                    qsar.stages << "visualization";
            }

            // legacy 兼容输出: 从合并产品按列切片生成逐 IW flat/diff
            if (mergedOk && mParams.legacyPerIwOutputs) {
                QDir().mkpath(outputDir + "/legacy_iw");
                int cumCol = 0;
                for (int i = 0; i < metas.size(); ++i) {
                    const int wCols = geomTable.swaths[i].width;
                    const QString iwName = QStringLiteral("%1_%2")
                        .arg(metas[i].swath).arg(pol);
                    const QString dstBase = outputDir + "/legacy_iw/" + iwName;
                    if (!fctx.outputBand.flatFile.isEmpty()) {
                        sliceColumns(fctx.flatSourcePath,
                            dstBase + "_flat.tif", cumCol, wCols);
                        sliceColumns(fctx.outputBand.flatPhaseFile.isEmpty()
                            ? QString() : flatDir + "/S1_" + pol + "_flat_phase.tif",
                            dstBase + "_flat_phase.tif", cumCol, wCols);
                    }
                    if (!fctx.outputBand.diffFile.isEmpty()) {
                        sliceColumns(diffDir + "/S1_" + pol + "_diff.tif",
                            dstBase + "_diff.tif", cumCol, wCols);
                        sliceColumns(diffDir + "/S1_" + pol + "_diff_phase.tif",
                            dstBase + "_diff_phase.tif", cumCol, wCols);
                    }
                    cumCol += wCols;
                }
            }

            // ── 合并产品 QSAR 波段 (可见: phase_color + coh; 中间体隐藏) ──
            QsarBand qbM;
            qbM.subSwath = "IW"; qbM.polarization = pol;
            qbM.file = QStringLiteral("merge/S1_%1_phase.tif").arg(pol);
            qbM.phaseFile = qbM.file;
            qbM.ifgFile = QStringLiteral("merge/S1_%1_ifg.tif").arg(pol);
            qbM.cohFile = QStringLiteral("merge/S1_%1_coh.tif").arg(pol);
            qbM.flatFile = fctx.outputBand.flatFile;
            qbM.flatPhaseFile = fctx.outputBand.flatPhaseFile;
            qbM.diffFile = fctx.outputBand.diffFile;
            qbM.diffPhaseFile = fctx.outputBand.diffPhaseFile;
            qbM.layerType = "phase";
            qbM.defaultVisible = false;
            {
                GdalSlcReader dimRdr;
                if (dimRdr.open(mergeBase + "_ifg.tif")) {
                    qbM.width = dimRdr.width();
                    qbM.height = dimRdr.height();
                    dimRdr.close();
                }
            }
            qsar.bands.append(qbM);
            // 彩色渲染图层 (默认可见)
            if (visOk) {
                QsarBand qbColor;
                qbColor.subSwath = "IW"; qbColor.polarization = pol;
                qbColor.file = QStringLiteral("visualization/S1_%1_phase_color.tif").arg(pol);
                qbColor.layerType = "phase_color";
                qbColor.defaultVisible = true;
                qbColor.width = qbM.width;
                qbColor.height = qbM.height;
                qsar.bands.append(qbColor);
            }
            // 相干图层 (可见)
            QsarBand qbMCoh;
            qbMCoh.subSwath = "IW"; qbMCoh.polarization = pol;
            qbMCoh.file = QStringLiteral("merge/S1_%1_coh.tif").arg(pol);
            qbMCoh.cohFile = qbMCoh.file;
            qbMCoh.layerType = "coherence";
            qbMCoh.defaultVisible = true;
            qsar.bands.append(qbMCoh);
            // flat/diff 中间体条目 (隐藏)
            if (!fctx.outputBand.flatPhaseFile.isEmpty()) {
                QsarBand qbF;
                qbF.subSwath = "IW"; qbF.polarization = pol;
                qbF.file = fctx.outputBand.flatPhaseFile;
                qbF.flatPhaseFile = qbF.file;
                qbF.flatFile = fctx.outputBand.flatFile;
                qbF.layerType = "flat_phase";
                qbF.defaultVisible = false;
                qsar.bands.append(qbF);
            }
            if (!fctx.outputBand.diffPhaseFile.isEmpty()) {
                QsarBand qbD;
                qbD.subSwath = "IW"; qbD.polarization = pol;
                qbD.file = fctx.outputBand.diffPhaseFile;
                qbD.diffPhaseFile = qbD.file;
                qbD.diffFile = fctx.outputBand.diffFile;
                qbD.layerType = "diff_phase";
                qbD.defaultVisible = false;
                qsar.bands.append(qbD);
            }
            // 差分彩色图层 (隐藏)
            if (visOk && QFileInfo::exists(outputDir + "/visualization/S1_"
                                           + pol + "_diff_color.tif")) {
                QsarBand qbDC;
                qbDC.subSwath = "IW"; qbDC.polarization = pol;
                qbDC.file = QStringLiteral("visualization/S1_%1_diff_color.tif").arg(pol);
                qbDC.layerType = "diff_color";
                qbDC.defaultVisible = false;
                qsar.bands.append(qbDC);
            }
        }
    }

    QString qsarPath = outputDir + "/" + mParams.outputPrefix + ".qsar";
    QsarIO::write(qsarPath, qsar);

    emit progressChanged(100, QStringLiteral("干涉图生成完成 (%1/%2对)").arg(succeeded).arg(pairs.size()));
    emit finished(succeeded > 0, qsarPath);
    mRunning = false;
}
