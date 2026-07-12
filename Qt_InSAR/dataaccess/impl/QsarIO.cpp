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

    QJsonArray bands;
    for (const auto& b : product.bands) {
        QJsonObject bo;
        bo["subSwath"] = b.subSwath;
        bo["polarization"] = b.polarization;
        bo["file"] = b.file;
        bo["width"] = b.width;
        bo["height"] = b.height;
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

    QJsonArray bands = root["bands"].toArray();
    for (const auto& v : bands) {
        QJsonObject bo = v.toObject();
        QsarBand b;
        b.subSwath = bo["subSwath"].toString();
        b.polarization = bo["polarization"].toString();
        b.file = bo["file"].toString();
        b.width = bo["width"].toInt();
        b.height = bo["height"].toInt();
        p.bands.append(b);
    }

    // 将相对路径补全为绝对路径
    QDir dir = QFileInfo(filePath).absoluteDir();
    for (auto& b : p.bands)
        b.file = dir.absoluteFilePath(b.file);

    return p;
}
