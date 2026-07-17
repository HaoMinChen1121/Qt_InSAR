#include "QsarIO.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

QString QsarIO::mLastError;

bool QsarIO::write(const QString& filePath, const QsarProduct& product)
{
    QJsonObject root;
    root["format"] = product.format;
    root["productType"] = product.productType;
    root["created"] = product.created;
    root["sourceMaster"] = product.sourceMaster;
    root["sourceSlave"] = product.sourceSlave;
    root["coarseMethod"] = product.coarseMethod;
    root["resamplingMethod"] = product.resamplingMethod;
    root["outputPrefix"] = product.outputPrefix;

    QJsonObject bl;
    bl["perpendicular"] = product.baseline.perpendicular;
    bl["parallel"] = product.baseline.parallel;
    bl["temporal"] = product.baseline.temporal;
    bl["ambiguityHeight"] = product.baseline.ambiguityHeight;
    root["baseline"] = bl;

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
        // burst metadata
        if (b.burstCount > 0) {
            bo["burstCount"] = b.burstCount;
            bo["linesPerBurst"] = b.linesPerBurst;
            bo["azimuthFrequency"] = b.azimuthFrequency;
            QJsonArray startLines;
            for (int v : b.burstStartLines) startLines.append(v);
            bo["burstStartLines"] = startLines;
            QJsonArray burstTimes;
            for (const auto& t : b.burstAzimuthTimes)
                burstTimes.append(t.toString(Qt::ISODate));
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
    p.productType = root["productType"].toString();
    p.created = root["created"].toString();
    p.sourceMaster = root["sourceMaster"].toString();
    p.sourceSlave = root["sourceSlave"].toString();
    p.coarseMethod = root["coarseMethod"].toString();
    p.resamplingMethod = root["resamplingMethod"].toString();
    p.outputPrefix = root["outputPrefix"].toString();

    QJsonObject bl = root["baseline"].toObject();
    p.baseline.perpendicular = bl["perpendicular"].toDouble();
    p.baseline.parallel = bl["parallel"].toDouble();
    p.baseline.temporal = bl["temporal"].toDouble();
    p.baseline.ambiguityHeight = bl["ambiguityHeight"].toDouble();

    QJsonArray stages = root["stages"].toArray();
    for (const auto& s : stages) p.stages.append(s.toString());

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
        // burst metadata
        if (bo.contains("burstCount")) {
            b.burstCount = bo["burstCount"].toInt();
            b.linesPerBurst = bo["linesPerBurst"].toInt();
            b.azimuthFrequency = bo["azimuthFrequency"].toDouble();
            for (const auto& v : bo["burstStartLines"].toArray())
                b.burstStartLines.append(v.toInt());
            for (const auto& v : bo["burstAzimuthTimes"].toArray())
                b.burstAzimuthTimes.append(QDateTime::fromString(v.toString(), Qt::ISODate));
        }
        p.bands.append(b);
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

    return p;
}
