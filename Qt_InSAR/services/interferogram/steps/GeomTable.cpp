#include "GeomTable.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <cmath>
#include <algorithm>

bool GeomTable::load(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[GeomTable] cannot open" << path;
        return false;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[GeomTable] invalid JSON" << path << err.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    width = root.value(QStringLiteral("width")).toInt();
    swaths.clear();
    for (const auto& v : root.value(QStringLiteral("swaths")).toArray()) {
        const QJsonObject o = v.toObject();
        SwathGeom g;
        g.name = o.value(QStringLiteral("name")).toString();
        g.startCol = o.value(QStringLiteral("startCol")).toInt();
        g.width = o.value(QStringLiteral("width")).toInt();
        g.nearRange = o.value(QStringLiteral("nearRange")).toDouble();
        g.rangeSpacing = o.value(QStringLiteral("rangeSpacing")).toDouble();
        swaths.append(g);
    }
    qDebug() << "[GeomTable] loaded" << path << "width=" << width
             << "swaths=" << swaths.size();
    return width > 0 && !swaths.isEmpty();
}

bool GeomTable::save(const QString& path) const
{
    QJsonObject root;
    root.insert(QStringLiteral("width"), width);
    QJsonArray arr;
    for (const auto& g : swaths) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), g.name);
        o.insert(QStringLiteral("startCol"), g.startCol);
        o.insert(QStringLiteral("width"), g.width);
        o.insert(QStringLiteral("nearRange"), g.nearRange);
        o.insert(QStringLiteral("rangeSpacing"), g.rangeSpacing);
        arr.append(o);
    }
    root.insert(QStringLiteral("swaths"), arr);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[GeomTable] cannot write" << path;
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    qDebug() << "[GeomTable] saved" << path << "width=" << width
             << "swaths=" << swaths.size();
    return true;
}

bool GeomTable::colGeometry(int col, double* range, double* incidenceRad) const
{
    if (col < 0 || col >= width) return false;
    const SwathGeom* g = nullptr;
    for (const auto& s : swaths) {
        if (col >= s.startCol && col < s.startCol + s.width) { g = &s; break; }
    }
    if (!g || g->rangeSpacing <= 0 || g->nearRange <= 0) return false;

    const double R = g->nearRange + (col - g->startCol) * g->rangeSpacing;
    // 几何推导入射角: cosθ = (R² + 2·H·Re + H²) / (2·R·(H+Re))
    const double H = kPlatformHeight, Re = kEarthRadius;
    const double cosT = (R * R + 2.0 * H * Re + H * H) / (2.0 * R * (H + Re));
    const double theta = std::acos(std::clamp(cosT, 0.0, 1.0));

    if (range) *range = R;
    if (incidenceRad) *incidenceRad = theta;
    return true;
}
