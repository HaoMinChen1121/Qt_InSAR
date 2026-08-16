#include "QsarIO.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QDebug>

QString QsarIO::mLastError;
const char* QsarIO::kSoftwareVersion = "2.0";

namespace {

QJsonObject orbitToJson(const QsarOrbitMeta& o)
{
    QJsonObject jo;
    if (!o.source.isEmpty()) jo["source"] = o.source;
    if (!o.direction.isEmpty()) jo["direction"] = o.direction;
    if (!o.referenceTime.isEmpty()) jo["referenceTime"] = o.referenceTime;
    QJsonArray svs;
    for (const auto& sv : o.stateVectors) {
        QJsonObject so;
        so["t"] = sv.utcTime.toString(Qt::ISODate);
        so["pos"] = QJsonArray{ sv.x, sv.y, sv.z };
        so["vel"] = QJsonArray{ sv.vx, sv.vy, sv.vz };
        svs.append(so);
    }
    if (!svs.isEmpty()) jo["stateVectors"] = svs;
    return jo;
}

QsarOrbitMeta orbitFromJson(const QJsonObject& jo)
{
    QsarOrbitMeta o;
    o.source = jo["source"].toString();
    o.direction = jo["direction"].toString();
    o.referenceTime = jo["referenceTime"].toString();
    const QJsonArray svs = jo["stateVectors"].toArray();
    QDateTime t0;
    bool first = true;
    for (const auto& v : svs) {
        const QJsonObject so = v.toObject();
        OrbitStateVector sv;
        sv.utcTime = QDateTime::fromString(so["t"].toString(), Qt::ISODate);
        const QJsonArray pos = so["pos"].toArray();
        const QJsonArray vel = so["vel"].toArray();
        if (pos.size() >= 3) { sv.x = pos[0].toDouble(); sv.y = pos[1].toDouble(); sv.z = pos[2].toDouble(); }
        if (vel.size() >= 3) { sv.vx = vel[0].toDouble(); sv.vy = vel[1].toDouble(); sv.vz = vel[2].toDouble(); }
        if (first) {
            t0 = sv.utcTime;
            sv.relativeTime = 0.0;
            first = false;
        } else {
            sv.relativeTime = t0.isValid() ? t0.msecsTo(sv.utcTime) / 1000.0 : 0.0;
        }
        o.stateVectors.append(sv);
    }
    return o;
}

} // namespace

bool QsarIO::write(const QString& filePath, const QsarProduct& product)
{
    QJsonObject root;
    root["format"] = product.format;
    root["schemaVersion"] = product.schemaVersion.isEmpty()
        ? QStringLiteral("2.0") : product.schemaVersion;
    root["productType"] = product.productType;
    root["created"] = product.created;
    root["sourceMaster"] = product.sourceMaster;
    root["sourceSlave"] = product.sourceSlave;
    if (!product.coarseMethod.isEmpty()) root["coarseMethod"] = product.coarseMethod;
    if (!product.resamplingMethod.isEmpty()) root["resamplingMethod"] = product.resamplingMethod;
    if (!product.outputPrefix.isEmpty()) root["outputPrefix"] = product.outputPrefix;

    // ═══ metadata (schema v2.0) ═══
    QJsonObject meta;
    const ProductMetadata& m = product.metadata;

    // pair
    if (!m.pair.masterId.isEmpty() || !m.pair.slaveId.isEmpty()
        || !m.pair.masterTime.isEmpty() || !m.pair.slaveTime.isEmpty()) {
        QJsonObject po;
        po["masterId"] = m.pair.masterId;
        po["slaveId"] = m.pair.slaveId;
        if (!m.pair.masterTime.isEmpty()) po["masterTime"] = m.pair.masterTime;
        if (!m.pair.slaveTime.isEmpty()) po["slaveTime"] = m.pair.slaveTime;
        meta["pair"] = po;
    }

    // orbit (master/slave)
    {
        const QJsonObject om = orbitToJson(m.orbitMaster);
        const QJsonObject os = orbitToJson(m.orbitSlave);
        if (!om.isEmpty() || !os.isEmpty()) {
            QJsonObject orb;
            if (!om.isEmpty()) orb["master"] = om;
            if (!os.isEmpty()) orb["slave"] = os;
            meta["orbit"] = orb;
        }
    }

    // baseline (pair 级)
    if (m.baseline.valid) {
        QJsonObject bl;
        bl["perpendicular"] = m.baseline.perpendicular;
        bl["parallel"] = m.baseline.parallel;
        bl["temporal"] = m.baseline.temporal;
        bl["ambiguityHeight"] = m.baseline.ambiguityHeight;
        meta["baseline"] = bl;
    }

    // processing
    {
        QJsonObject proc;
        if (m.processing.hasRegistration) {
            const QsarRegistrationMeta& rp = m.processing.registration;
            QJsonObject r;
            r["coarseMethod"] = rp.coarseMethod;
            r["coarseWindowSize"] = rp.coarseWindowSize;
            r["coarseSearchWindow"] = rp.coarseSearchWindow;
            r["fineWindowSize"] = rp.fineWindowSize;
            r["polynomialDegree"] = rp.polynomialDegree;
            r["correlationThreshold"] = rp.correlationThreshold;
            r["resamplingMethod"] = rp.resamplingMethod;
            r["sincWindowSize"] = rp.sincWindowSize;
            proc["registration"] = r;
        }
        if (m.processing.hasInterferogram) {
            const QsarInterferogramMeta& fp = m.processing.interferogram;
            QJsonObject f;
            f["rangeLooks"] = fp.rangeLooks;
            f["azimuthLooks"] = fp.azimuthLooks;
            f["wavelength"] = fp.wavelength;
            f["inputRangeSpacing"] = fp.inputRangeSpacing;
            f["inputAzimuthSpacing"] = fp.inputAzimuthSpacing;
            f["outputRangeSpacing"] = fp.outputRangeSpacing;
            f["outputAzimuthSpacing"] = fp.outputAzimuthSpacing;
            proc["interferogram"] = f;
        }
        if (!proc.isEmpty()) meta["processing"] = proc;
    }

    // geometry 摘要
    if (!m.geometry.model.isEmpty()) {
        QJsonObject g;
        g["model"] = m.geometry.model;
        g["file"] = m.geometry.file;
        g["nearRange"] = m.geometry.nearRange;
        g["farRange"] = m.geometry.farRange;
        g["incMin"] = m.geometry.incMin;
        g["incMax"] = m.geometry.incMax;
        meta["geometry"] = g;
    }

    // quality 摘要
    {
        const QsarQualityMeta& q = m.quality;
        if (q.meanCorrelation > 0 || q.offsetRmse > 0 || q.validRatio > 0
            || q.meanCoherence > 0 || q.unwrapReady || !q.detailFile.isEmpty()) {
            QJsonObject qo;
            qo["meanCorrelation"] = q.meanCorrelation;
            qo["offsetRmse"] = q.offsetRmse;
            qo["validRatio"] = q.validRatio;
            qo["meanCoherence"] = q.meanCoherence;
            qo["unwrapReady"] = q.unwrapReady;
            if (!q.detailFile.isEmpty()) qo["detailFile"] = q.detailFile;
            meta["quality"] = qo;
        }
    }

    // tops (canonical burst 元数据)
    if (!m.tops.swaths.isEmpty()) {
        QJsonArray sws;
        for (const auto& s : m.tops.swaths) {
            QJsonObject so;
            so["name"] = s.name;
            so["burstCount"] = s.burstCount;
            so["linesPerBurst"] = s.linesPerBurst;
            so["azimuthFrequency"] = s.azimuthFrequency;
            QJsonArray bs;
            for (const auto& b : s.bursts) {
                QJsonObject bo;
                bo["index"] = b.index;
                bo["startLine"] = b.startLine;
                bo["azimuthTime"] = b.azimuthTime;
                bo["esdCorrection"] = b.esdCorrection;
                if (!b.dcPoly.isEmpty()) {
                    QJsonArray dc;
                    for (double v : b.dcPoly) dc.append(v);
                    bo["dcPoly"] = dc;
                    bo["dcT0"] = b.dcT0;
                }
                bs.append(bo);
            }
            so["bursts"] = bs;
            sws.append(so);
        }
        QJsonObject tops;
        tops["swaths"] = sws;
        meta["tops"] = tops;
    }

    if (!meta.isEmpty()) root["metadata"] = meta;

    // history (v2 结构化) + stages (legacy 名称数组, 保持兼容)
    {
        QJsonArray hist;
        for (const auto& h : product.history) {
            QJsonObject ho;
            ho["name"] = h.name;
            ho["time"] = h.time;
            if (!h.softwareVersion.isEmpty()) ho["softwareVersion"] = h.softwareVersion;
            if (!h.params.isEmpty()) ho["params"] = QJsonObject::fromVariantMap(h.params);
            hist.append(ho);
        }
        if (!hist.isEmpty()) root["history"] = hist;
    }
    QJsonArray stages;
    for (const auto& s : product.stages) stages.append(s);
    root["stages"] = stages;

    QJsonArray bands;
    for (const auto& b : product.bands) {
        QJsonObject bo;
        bo["subSwath"] = b.subSwath;
        bo["polarization"] = b.polarization;
        bo["file"] = b.file;
        bo["width"] = b.width; bo["height"] = b.height;
        if (!b.ifgFile.isEmpty()) bo["ifg"] = b.ifgFile;
        if (!b.cohFile.isEmpty()) bo["coh"] = b.cohFile;
        if (!b.phaseFile.isEmpty()) bo["phase"] = b.phaseFile;
        if (!b.flatFile.isEmpty()) bo["flat"] = b.flatFile;
        if (!b.flatPhaseFile.isEmpty()) bo["flat_phase"] = b.flatPhaseFile;
        if (!b.diffFile.isEmpty()) bo["diff"] = b.diffFile;
        if (!b.diffPhaseFile.isEmpty()) bo["diff_phase"] = b.diffPhaseFile;
        // 显示控制
        bo["layerType"] = b.layerType;
        bo["defaultVisible"] = b.defaultVisible;
        // burst metadata (legacy 兼容位置, canonical 在 metadata.tops)
        if (b.burstCount > 0) {
            bo["burstCount"] = b.burstCount;
            bo["linesPerBurst"] = b.linesPerBurst;
            bo["azimuthFrequency"] = b.azimuthFrequency;
            QJsonArray startLines;
            for (int v : b.burstStartLines) startLines.append(v);
            bo["burstStartLines"] = startLines;
            QJsonArray burstTimes;
            for (const auto& t : b.burstAzimuthTimes)
                burstTimes.append(t.toString(Qt::ISODateWithMs));
            bo["burstAzimuthTimes"] = burstTimes;
        }
        bands.append(bo);
    }
    root["bands"] = bands;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        mLastError = QStringLiteral("无法写入: %1").arg(filePath);
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

QsarProduct QsarIO::read(const QString& filePath)
{
    QsarProduct p;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        mLastError = QStringLiteral("无法打开: %1").arg(filePath);
        return p;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        mLastError = QStringLiteral("JSON解析错误: %1").arg(err.errorString());
        return p;
    }

    QJsonObject root = doc.object();
    p.format = root["format"].toString();
    p.schemaVersion = root["schemaVersion"].toString();
    if (p.schemaVersion.isEmpty()) p.schemaVersion = QStringLiteral("1.0");
    p.productType = root["productType"].toString();
    p.created = root["created"].toString();
    p.sourceMaster = root["sourceMaster"].toString();
    p.sourceSlave = root["sourceSlave"].toString();
    p.coarseMethod = root["coarseMethod"].toString();
    p.resamplingMethod = root["resamplingMethod"].toString();
    p.outputPrefix = root["outputPrefix"].toString();

    // legacy v1 顶层 baseline (兼容读取)
    if (root.contains("baseline")) {
        QJsonObject bl = root["baseline"].toObject();
        p.baseline.perpendicular = bl["perpendicular"].toDouble();
        p.baseline.parallel = bl["parallel"].toDouble();
        p.baseline.temporal = bl["temporal"].toDouble();
        p.baseline.ambiguityHeight = bl["ambiguityHeight"].toDouble();
    }

    // stages (legacy) + history (v2)
    {
        QJsonArray stages = root["stages"].toArray();
        for (const auto& s : stages) p.stages.append(s.toString());
        QJsonArray hist = root["history"].toArray();
        for (const auto& v : hist) {
            QJsonObject ho = v.toObject();
            QsarStageRecord r;
            r.name = ho["name"].toString();
            r.time = ho["time"].toString();
            r.softwareVersion = ho["softwareVersion"].toString();
            r.params = ho["params"].toObject().toVariantMap();
            p.history.append(r);
        }
        if (p.history.isEmpty()) {
            for (const auto& s : p.stages) {
                QsarStageRecord r;
                r.name = s;
                p.history.append(r);
            }
        }
    }

    QJsonArray bands = root["bands"].toArray();
    for (const auto& v : bands) {
        QJsonObject bo = v.toObject();
        QsarBand b;
        b.subSwath = bo["subSwath"].toString();
        b.polarization = bo["polarization"].toString();
        b.file = bo["file"].toString();
        b.width = bo["width"].toInt(); b.height = bo["height"].toInt();
        b.ifgFile = bo["ifg"].toString();
        b.cohFile = bo["coh"].toString();
        b.phaseFile = bo["phase"].toString();
        b.flatFile = bo["flat"].toString();
        b.flatPhaseFile = bo["flat_phase"].toString();
        b.diffFile = bo["diff"].toString();
        b.diffPhaseFile = bo["diff_phase"].toString();
        // 显示属性
        b.layerType = bo["layerType"].toString("phase");
        b.defaultVisible = bo["defaultVisible"].toBool(false);
        // burst metadata
        if (bo.contains("burstCount")) {
            b.burstCount = bo["burstCount"].toInt();
            b.linesPerBurst = bo["linesPerBurst"].toInt();
            b.azimuthFrequency = bo["azimuthFrequency"].toDouble();
            for (const auto& v : bo["burstStartLines"].toArray())
                b.burstStartLines.append(v.toInt());
            for (const auto& v : bo["burstAzimuthTimes"].toArray())
                b.burstAzimuthTimes.append(QDateTime::fromString(v.toString(), Qt::ISODateWithMs));
        }
        p.bands.append(b);
    }

    // ═══ metadata (schema v2.0) ═══
    ProductMetadata& m = p.metadata;
    if (root.contains("metadata")) {
        QJsonObject meta = root["metadata"].toObject();

        if (meta.contains("pair")) {
            QJsonObject po = meta["pair"].toObject();
            m.pair.masterId = po["masterId"].toString();
            m.pair.slaveId = po["slaveId"].toString();
            m.pair.masterTime = po["masterTime"].toString();
            m.pair.slaveTime = po["slaveTime"].toString();
        }

        if (meta.contains("orbit")) {
            QJsonObject orb = meta["orbit"].toObject();
            m.orbitMaster = orbitFromJson(orb["master"].toObject());
            m.orbitSlave = orbitFromJson(orb["slave"].toObject());
        }

        if (meta.contains("baseline")) {
            QJsonObject bl = meta["baseline"].toObject();
            m.baseline.valid = true;
            m.baseline.perpendicular = bl["perpendicular"].toDouble();
            m.baseline.parallel = bl["parallel"].toDouble();
            m.baseline.temporal = bl["temporal"].toDouble();
            m.baseline.ambiguityHeight = bl["ambiguityHeight"].toDouble();
            // 同步 legacy 字段 (兼容仍读顶层 baseline 的代码)
            p.baseline.perpendicular = m.baseline.perpendicular;
            p.baseline.parallel = m.baseline.parallel;
            p.baseline.temporal = m.baseline.temporal;
            p.baseline.ambiguityHeight = m.baseline.ambiguityHeight;
        } else if (root.contains("baseline")) {
            // v1 → v2 迁移: 顶层 baseline 非全零时视为有效
            const bool any = p.baseline.perpendicular != 0 || p.baseline.parallel != 0
                || p.baseline.temporal != 0 || p.baseline.ambiguityHeight != 0;
            m.baseline.valid = any;
            m.baseline.perpendicular = p.baseline.perpendicular;
            m.baseline.parallel = p.baseline.parallel;
            m.baseline.temporal = p.baseline.temporal;
            m.baseline.ambiguityHeight = p.baseline.ambiguityHeight;
        }

        if (meta.contains("processing")) {
            QJsonObject proc = meta["processing"].toObject();
            if (proc.contains("registration")) {
                QJsonObject r = proc["registration"].toObject();
                m.processing.hasRegistration = true;
                m.processing.registration.coarseMethod = r["coarseMethod"].toString();
                m.processing.registration.coarseWindowSize = r["coarseWindowSize"].toInt();
                m.processing.registration.coarseSearchWindow = r["coarseSearchWindow"].toInt();
                m.processing.registration.fineWindowSize = r["fineWindowSize"].toInt();
                m.processing.registration.polynomialDegree = r["polynomialDegree"].toInt();
                m.processing.registration.correlationThreshold = r["correlationThreshold"].toDouble();
                m.processing.registration.resamplingMethod = r["resamplingMethod"].toString();
                m.processing.registration.sincWindowSize = r["sincWindowSize"].toInt();
            }
            if (proc.contains("interferogram")) {
                QJsonObject f = proc["interferogram"].toObject();
                m.processing.hasInterferogram = true;
                m.processing.interferogram.rangeLooks = f["rangeLooks"].toInt();
                m.processing.interferogram.azimuthLooks = f["azimuthLooks"].toInt();
                m.processing.interferogram.wavelength = f["wavelength"].toDouble();
                m.processing.interferogram.inputRangeSpacing = f["inputRangeSpacing"].toDouble();
                m.processing.interferogram.inputAzimuthSpacing = f["inputAzimuthSpacing"].toDouble();
                m.processing.interferogram.outputRangeSpacing = f["outputRangeSpacing"].toDouble();
                m.processing.interferogram.outputAzimuthSpacing = f["outputAzimuthSpacing"].toDouble();
            }
        }

        if (meta.contains("geometry")) {
            QJsonObject g = meta["geometry"].toObject();
            m.geometry.model = g["model"].toString();
            m.geometry.file = g["file"].toString();
            m.geometry.nearRange = g["nearRange"].toDouble();
            m.geometry.farRange = g["farRange"].toDouble();
            m.geometry.incMin = g["incMin"].toDouble();
            m.geometry.incMax = g["incMax"].toDouble();
        }

        if (meta.contains("quality")) {
            QJsonObject qo = meta["quality"].toObject();
            m.quality.meanCorrelation = qo["meanCorrelation"].toDouble();
            m.quality.offsetRmse = qo["offsetRmse"].toDouble();
            m.quality.validRatio = qo["validRatio"].toDouble();
            m.quality.meanCoherence = qo["meanCoherence"].toDouble();
            m.quality.unwrapReady = qo["unwrapReady"].toBool();
            m.quality.detailFile = qo["detailFile"].toString();
        }

        if (meta.contains("tops")) {
            QJsonArray sws = meta["tops"].toObject()["swaths"].toArray();
            for (const auto& v : sws) {
                QJsonObject so = v.toObject();
                QsarTopsSwath s;
                s.name = so["name"].toString();
                s.burstCount = so["burstCount"].toInt();
                s.linesPerBurst = so["linesPerBurst"].toInt();
                s.azimuthFrequency = so["azimuthFrequency"].toDouble();
                for (const auto& bv : so["bursts"].toArray()) {
                    QJsonObject bo = bv.toObject();
                    QsarTopsBurst t;
                    t.index = bo["index"].toInt();
                    t.startLine = bo["startLine"].toInt();
                    t.azimuthTime = bo["azimuthTime"].toString();
                    t.esdCorrection = bo["esdCorrection"].toDouble();
                    for (const auto& dv : bo["dcPoly"].toArray())
                        t.dcPoly.append(dv.toDouble());
                    t.dcT0 = bo["dcT0"].toDouble();
                    s.bursts.append(t);
                }
                m.tops.swaths.append(s);
            }
        }
    }

    // v1 → v2 迁移: bands 的 burst 字段 → metadata.tops (仅当 tops 缺失)
    if (m.tops.swaths.isEmpty()) {
        QHash<QString, bool> seen;
        for (const auto& b : p.bands) {
            if (b.burstCount <= 0 || seen.contains(b.subSwath)) continue;
            seen.insert(b.subSwath, true);
            QsarTopsSwath s;
            s.name = b.subSwath;
            s.burstCount = b.burstCount;
            s.linesPerBurst = b.linesPerBurst;
            s.azimuthFrequency = b.azimuthFrequency;
            for (int i = 0; i < b.burstStartLines.size(); ++i) {
                QsarTopsBurst t;
                t.index = i;
                t.startLine = b.burstStartLines[i];
                if (i < b.burstAzimuthTimes.size())
                    t.azimuthTime = b.burstAzimuthTimes[i].toString(Qt::ISODateWithMs);
                s.bursts.append(t);
            }
            m.tops.swaths.append(s);
        }
    }

    // 将相对路径补全为绝对路径
    QDir dir = QFileInfo(filePath).absoluteDir();
    for (auto& b : p.bands) {
        b.file = dir.absoluteFilePath(b.file);
        if (!b.ifgFile.isEmpty()) b.ifgFile = dir.absoluteFilePath(b.ifgFile);
        if (!b.cohFile.isEmpty()) b.cohFile = dir.absoluteFilePath(b.cohFile);
        if (!b.phaseFile.isEmpty()) b.phaseFile = dir.absoluteFilePath(b.phaseFile);
        if (!b.flatFile.isEmpty()) b.flatFile = dir.absoluteFilePath(b.flatFile);
        if (!b.flatPhaseFile.isEmpty()) b.flatPhaseFile = dir.absoluteFilePath(b.flatPhaseFile);
        if (!b.diffFile.isEmpty()) b.diffFile = dir.absoluteFilePath(b.diffFile);
        if (!b.diffPhaseFile.isEmpty()) b.diffPhaseFile = dir.absoluteFilePath(b.diffPhaseFile);
    }
    if (!m.geometry.file.isEmpty()) m.geometry.file = dir.absoluteFilePath(m.geometry.file);
    if (!m.quality.detailFile.isEmpty()) m.quality.detailFile = dir.absoluteFilePath(m.quality.detailFile);

    return p;
}
