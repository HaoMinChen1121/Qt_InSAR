#include "TiffTiler.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <gdal_priv.h>

QString TiffTiler::ensureTiled(const QString& slcPath, const QString& cacheDir)
{
    QFileInfo fi(slcPath);
    QString cacheName = fi.completeBaseName() + "_tiled.tif";
    QString tiledPath = QDir::cleanPath(cacheDir + "/" + cacheName);

    if (QFileInfo::exists(tiledPath))
        return tiledPath;

    QDir().mkpath(cacheDir);
    qDebug() << "[TiffTiler] creating tiled TIFF:" << slcPath << "->" << tiledPath;

    GDALDatasetH hSrc = GDALOpen(slcPath.toUtf8().constData(), GA_ReadOnly);
    if (!hSrc) {
        qWarning() << "[TiffTiler] cannot open:" << slcPath;
        return slcPath; // fallback
    }

    GDALDriverH drv = GDALGetDriverByName("GTiff");
    if (!drv) { GDALClose(hSrc); return slcPath; }

    const char* opts[] = {"TILED=YES", "BLOCKXSIZE=256", "BLOCKYSIZE=256",
                          "COMPRESS=NONE", nullptr};
    GDALDatasetH hDst = GDALCreateCopy(drv, tiledPath.toUtf8().constData(),
                                        hSrc, FALSE, (char**)opts, nullptr, nullptr);
    GDALClose(hSrc);
    if (!hDst) {
        qWarning() << "[TiffTiler] create failed, fallback to original";
        return slcPath;
    }
    GDALClose(hDst);
    qDebug() << "[TiffTiler] tiled TIFF ready:" << tiledPath;
    return tiledPath;
}

void TiffTiler::cleanupCache(const QString& cacheDir)
{
    qDebug() << "[TiffTiler] cleanup:" << cacheDir;
    QDir(cacheDir).removeRecursively();
}
