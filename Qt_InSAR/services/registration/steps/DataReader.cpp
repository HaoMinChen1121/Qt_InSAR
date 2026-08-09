#include "DataReader.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

// ── 退出时清理的解压目录 ──
static QSet<QString>& extractedDirs() {
    static QSet<QString> dirs;
    return dirs;
}

// ── /vsizip 路径 → 解压到ZIP同目录, 返回本地路径 ──
static QString extractVsizip(const QString& vsiPath)
{
    // 格式: /vsizip/F:/path/to/file.zip/internal/path
    if (!vsiPath.startsWith("/vsizip/")) return vsiPath;

    QString inner = vsiPath.mid(8);
    int zipEnd = inner.indexOf(".zip/", 0, Qt::CaseInsensitive);
    if (zipEnd < 0) return vsiPath;
    QString zipPath  = inner.left(zipEnd + 4);
    QString innerPath = inner.mid(zipEnd + 5);
    int safeEnd = innerPath.indexOf('/');
    QString safeName = (safeEnd > 0) ? innerPath.left(safeEnd) : innerPath;

    // 解压到ZIP同目录:  F:/data/xxx.zip → F:/data/xxx_extracted/
    QFileInfo zipInfo(zipPath);
    QString extractDir = zipInfo.absolutePath() + "/" + zipInfo.completeBaseName() + "_extracted";
    QString safeDir = extractDir + "/" + safeName;

    if (!QFileInfo::exists(safeDir)) {
        QDir().mkpath(extractDir);
        qDebug() << "[DataReader] extracting" << zipPath << "->" << extractDir;

        QProcess proc;
        proc.start("tar", {"-xf", zipPath, "-C", extractDir});
        if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
            qWarning() << "[DataReader] tar extract failed, fallback to /vsizip";
            QDir(extractDir).removeRecursively();
            return vsiPath;
        }
        qDebug() << "[DataReader] extract done";
        extractedDirs().insert(QDir::cleanPath(extractDir));
    }

    if (safeEnd > 0 && safeEnd + 1 < innerPath.length())
        return QDir::cleanPath(safeDir + "/" + innerPath.mid(safeEnd + 1));
    return QDir::cleanPath(safeDir);
}

bool DataReader::execute(PipelineContext& ctx) {
    // ── ZIP 预解压: /vsizip → 临时目录 ──
    QString masterLocal = extractVsizip(ctx.masterBand->rasterPath);
    QString slaveLocal  = extractVsizip(ctx.slaveBand->rasterPath);

    qDebug() << QStringLiteral("[DataReader] opening master: %1").arg(masterLocal);
    auto* mR = new GdalSlcReader();
    if (!mR->open(masterLocal)) {
        ctx.errorMessage = QStringLiteral("DataReader: master open fail");
        delete mR; return false;
    }
    qDebug() << "[DataReader] master opened ok";

    qDebug() << QStringLiteral("[DataReader] opening slave: %1").arg(slaveLocal);
    auto* sR = new GdalSlcReader();
    if (!sR->open(slaveLocal)) {
        ctx.errorMessage = QStringLiteral("DataReader: slave open fail");
        delete mR; delete sR; return false;
    }
    qDebug() << "[DataReader] slave opened ok";

    // 存储解压后的本地路径, 后续步骤从此读取
    ctx.masterLocalPath = masterLocal;
    ctx.slaveLocalPath  = slaveLocal;

    ctx.masterReader = mR;
    ctx.slaveReader  = sR;

    auto& d = ctx.data;
    d.masterWidth  = mR->width();
    d.masterHeight = mR->height();
    d.slaveWidth   = sR->width();
    d.slaveHeight  = sR->height();
    d.burstCount    = ctx.masterBand->burstCount;
    d.linesPerBurst = ctx.masterBand->linesPerBurst;
    d.burstStartLines    = ctx.masterBand->burstStartLines;
    d.masterBurstTimes   = ctx.masterBand->burstAzimuthTimes;
    d.slaveBurstTimes    = ctx.slaveBand->burstAzimuthTimes;
    d.slaveAzimuthFmRate       = ctx.slaveBand->azimuthFmRate;
    d.slaveAzimuthSteeringRate = ctx.slaveBand->azimuthSteeringRate;
    d.masterAzimuthFrequency   = ctx.masterBand->azimuthFrequency;
    ctx.isTopsar = (d.burstCount > 1);
    return true;
}

void DataReader::cleanupExtracted() {
    for (const auto& dir : extractedDirs()) {
        qDebug() << "[DataReader] cleanup:" << dir;
        QDir(dir).removeRecursively();
    }
    extractedDirs().clear();
}
