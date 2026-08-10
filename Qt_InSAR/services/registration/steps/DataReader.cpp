#include "DataReader.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include "preprocess/TiffTiler.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTextStream>
#include <gdal_priv.h>
#include <cpl_vsi.h>
#include <cpl_conv.h>

// ── 退出时清理的解压目录 ──
static QSet<QString>& extractedDirs() {
    static QSet<QString> dirs;
    return dirs;
}

// ── GDAL VSI 复制文件 (顺序单线程, 线程安全) ──
static bool vsiCopyFile(const QString& srcVsi, const QString& dstPath) {
    VSILFILE* src = VSIFOpenExL(srcVsi.toUtf8().constData(), "rb", TRUE);
    if (!src) return false;
    VSILFILE* dst = VSIFOpenExL(dstPath.toUtf8().constData(), "wb", FALSE);
    if (!dst) { VSIFCloseL(src); return false; }

    QByteArray buf(1024 * 1024, Qt::Uninitialized); // 1MB buffer
    bool ok = true;
    while (true) {
        vsi_l_offset n = VSIFReadL(buf.data(), 1, buf.size(), src);
        if (n == 0) break;
        if (VSIFWriteL(buf.data(), 1, n, dst) != n) { ok = false; break; }
    }
    VSIFCloseL(src);
    VSIFCloseL(dst);
    return ok;
}

// ── 递归列出 /vsizip 内的所有文件 ──
static void listVsizipRecursive(const QString& dir, QStringList& files) {
    char** entries = VSIReadDir(dir.toUtf8().constData());
    if (!entries) return;
    for (int i = 0; entries[i]; ++i) {
        QString name = QString::fromUtf8(entries[i]);
        if (name == "." || name == "..") continue;
        QString full = dir + "/" + name;
        VSIStatBufL st;
        if (VSIStatL(full.toUtf8().constData(), &st) == 0) {
            if (VSI_ISDIR(st.st_mode))
                listVsizipRecursive(full, files);
            else
                files.append(full);
        }
    }
    CSLDestroy(entries);
}

// ── /vsizip → 提取到ZIP同目录 (非TOPSAR回退路径保留) ──
static QString extractVsizip(const QString& vsiPath)
{
    if (!vsiPath.startsWith("/vsizip/")) return vsiPath;

    QString inner = vsiPath.mid(8);
    int zipEnd = inner.indexOf(".zip/", 0, Qt::CaseInsensitive);
    if (zipEnd < 0) return vsiPath;
    QString zipPath  = inner.left(zipEnd + 4);
    QString innerPath = inner.mid(zipEnd + 5);
    int safeEnd = innerPath.indexOf('/');
    QString safeName = (safeEnd > 0) ? innerPath.left(safeEnd) : innerPath;

    QFileInfo zipInfo(zipPath);
    QString extractRoot = zipInfo.absolutePath() + "/" + zipInfo.completeBaseName() + "_extracted";
    QString safeDir = extractRoot + "/" + safeName;

    extractedDirs().insert(QDir::cleanPath(extractRoot));

    if (!QFileInfo::exists(safeDir)) {
        QDir().mkpath(extractRoot);
        qDebug() << "[DataReader] extracting (GDAL VSI)" << zipPath << "->" << extractRoot;

        QString vsiRoot = "/vsizip/" + zipPath;
        QStringList files;
        listVsizipRecursive(vsiRoot, files);

        for (const auto& vsiFile : files) {
            QString relPath = vsiFile.mid(vsiRoot.length() + 1);
            QString dstPath = extractRoot + "/" + relPath;
            QDir().mkpath(QFileInfo(dstPath).absolutePath());
            vsiCopyFile(vsiFile, dstPath);
        }
        qDebug() << "[DataReader] extract done:" << files.size() << "files";
    }

    if (safeEnd > 0 && safeEnd + 1 < innerPath.length())
        return QDir::cleanPath(safeDir + "/" + innerPath.mid(safeEnd + 1));
    return QDir::cleanPath(safeDir);
}

bool DataReader::execute(PipelineContext& ctx) {
    bool isTopsar = (ctx.masterBand->burstCount > 1);

    if (isTopsar) {
        auto parseVsiPath = [](const QString& vsiPath) -> QPair<QString,QString> {
            if (!vsiPath.startsWith("/vsizip/")) return {vsiPath, QString()};
            QString inner = vsiPath.mid(8);
            int zipEnd = inner.indexOf(".zip/", 0, Qt::CaseInsensitive);
            if (zipEnd < 0) return {vsiPath, QString()};
            return {inner.left(zipEnd + 4), inner.mid(zipEnd + 5)};
        };

        auto [masterZip, masterEntry] = parseVsiPath(ctx.masterBand->rasterPath);
        auto [slaveZip,  slaveEntry]  = parseVsiPath(ctx.slaveBand->rasterPath);

        qDebug() << QStringLiteral("[DataReader] SDR master zip=%1 entry=%2")
            .arg(masterZip, masterEntry);
        auto* mSdr = new SentinelDataReader();
        if (!mSdr->open(masterZip, masterEntry, *ctx.masterBand)) {
            ctx.errorMessage = QStringLiteral("DataReader: SentinelDataReader master open fail");
            delete mSdr; return false;
        }

        qDebug() << QStringLiteral("[DataReader] SDR slave zip=%1 entry=%2")
            .arg(slaveZip, slaveEntry);
        auto* sSdr = new SentinelDataReader();
        if (!sSdr->open(slaveZip, slaveEntry, *ctx.slaveBand)) {
            ctx.errorMessage = QStringLiteral("DataReader: SentinelDataReader slave open fail");
            delete mSdr; delete sSdr; return false;
        }

        ctx.masterSdr = mSdr;
        ctx.slaveSdr  = sSdr;
        ctx.useBurstCache = true;

        auto& d = ctx.data;
        d.masterWidth  = mSdr->width();
        d.masterHeight = mSdr->height();
        d.slaveWidth   = sSdr->width();
        d.slaveHeight  = sSdr->height();
        d.burstCount    = ctx.masterBand->burstCount;
        d.linesPerBurst = ctx.masterBand->linesPerBurst;
        d.samplesPerBurst = ctx.masterBand->samplesPerBurst;
        d.burstStartLines    = ctx.masterBand->burstStartLines;
        d.masterBurstTimes   = ctx.masterBand->burstAzimuthTimes;
        d.slaveBurstTimes    = ctx.slaveBand->burstAzimuthTimes;
        d.slaveAzimuthFmRate       = ctx.slaveBand->azimuthFmRate;
        d.slaveAzimuthSteeringRate = ctx.slaveBand->azimuthSteeringRate;
        d.masterAzimuthFrequency   = ctx.masterBand->azimuthFrequency;
        ctx.isTopsar = true;
        return true;
    }

    // ═══════════════════════════════════════════
    //  非TOPSAR 回退路径: 原有 extractVsizip + TiffTiler + GdalSlcReader
    // ═══════════════════════════════════════════
    QString masterLocal = extractVsizip(ctx.masterBand->rasterPath);
    QString slaveLocal  = extractVsizip(ctx.slaveBand->rasterPath);

    QString cacheDir = QFileInfo(masterLocal).absolutePath() + "/../_tiled_cache";
    extractedDirs().insert(QDir::cleanPath(cacheDir));
    masterLocal = TiffTiler::ensureTiled(masterLocal, cacheDir);
    slaveLocal  = TiffTiler::ensureTiled(slaveLocal, cacheDir);

    qDebug() << QStringLiteral("[DataReader] opening master: %1").arg(masterLocal);
    auto* mR = new GdalSlcReader();
    if (!mR->open(masterLocal)) {
        ctx.errorMessage = QStringLiteral("DataReader: master open fail");
        delete mR; return false;
    }

    qDebug() << QStringLiteral("[DataReader] opening slave: %1").arg(slaveLocal);
    auto* sR = new GdalSlcReader();
    if (!sR->open(slaveLocal)) {
        ctx.errorMessage = QStringLiteral("DataReader: slave open fail");
        delete mR; delete sR; return false;
    }

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
    QSet<QString> failed;
    for (const auto& dir : extractedDirs()) {
        QDir d(dir);
        QFileInfoList list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto& fi : list) {
            if (fi.isDir()) QDir(fi.absoluteFilePath()).removeRecursively();
            else { QFile f(fi.absoluteFilePath()); f.setPermissions(QFile::ReadOwner|QFile::WriteOwner); f.remove(); }
        }
        qDebug() << "[DataReader] cleanup:" << dir;
        if (QDir(dir).removeRecursively()) {
            qDebug() << "[DataReader] cleanup ok:" << dir;
        } else {
            qWarning() << "[DataReader] cleanup failed (locked):" << dir;
            failed.insert(dir);
        }
    }
    extractedDirs() = failed;
}
