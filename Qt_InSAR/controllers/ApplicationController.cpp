#include "ApplicationController.h"
#include "WorkerManager.h"
#include "mainwindow.h"
#include "ui/LayerPanel.h"
#include "ui/ProcessingMonitorPanel.h"
#include "ui/MapCanvasWidget.h"

#include "services/impl/RegistrationServiceImpl.h"
#include "services/impl/InterferogramServiceImpl.h"
#include "services/impl/FlatEarthServiceImpl.h"
#include "services/impl/DifferentialServiceImpl.h"
#include "services/impl/FilterServiceImpl.h"
#include "services/impl/UnwrappingServiceImpl.h"
#include "services/impl/GeocodingServiceImpl.h"

#include <qgsrasterlayer.h>
#include <qgslayertree.h>
#include <qgsproject.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDebug>
#include <QTextStream>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <gdal_priv.h>

ApplicationController::ApplicationController(MainWindow* mainWindow, QObject* parent)
    : QObject(parent), mMainWindow(mainWindow)
{
    mWorkerManager = new WorkerManager(this);
    createServices();
}

ApplicationController::~ApplicationController() {
    shutdown();
}

void ApplicationController::initialize()
{
    wireConnections();
}

void ApplicationController::createServices()
{
    mRegistrationSvc = std::make_unique<RegistrationServiceImpl>(this);
    mInterferogramSvc = std::make_unique<InterferogramServiceImpl>(this);
    mFlatEarthSvc = std::make_unique<FlatEarthServiceImpl>(this);
    mDifferentialSvc = std::make_unique<DifferentialServiceImpl>(this);
    mFilterSvc = std::make_unique<FilterServiceImpl>(this);
    mUnwrappingSvc = std::make_unique<UnwrappingServiceImpl>(this);
    mGeocodingSvc = std::make_unique<GeocodingServiceImpl>(this);
}

// Creates a VRT file that wraps a complex SLC source with an on-the-fly
// amplitude pixel function.  The VRT is ~1 KB of XML, created instantly —
// no pixel conversion happens until QGIS renders tiles.
static bool createAmplitudeVRT(const QString& vsiPath, const QString& vrtPath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return false;

    int w = GDALGetRasterXSize(srcDS);
    int h = GDALGetRasterYSize(srcDS);
    GDALRasterBandH srcBand = GDALGetRasterBand(srcDS, 1);
    QString srcTypeName = QString::fromUtf8(
        GDALGetDataTypeName(GDALGetRasterDataType(srcBand)));

    QFile f(vrtPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        GDALClose(srcDS);
        return false;
    }

    QTextStream ts(&f);
    ts << "<VRTDataset rasterXSize=\"" << w
       << "\" rasterYSize=\"" << h << "\">\n";

    const char* proj = GDALGetProjectionRef(srcDS);
    if (proj && strlen(proj) > 0)
        ts << "  <SRS><![CDATA[" << proj << "]]></SRS>\n";

    int nGCPs = GDALGetGCPCount(srcDS);
    if (nGCPs > 0) {
        const char* gcpProj = GDALGetGCPProjection(srcDS);
        QString escapedProj = QString::fromUtf8(gcpProj ? gcpProj : "");
        escapedProj.replace("&", "&amp;")
                   .replace("\"", "&quot;")
                   .replace("<", "&lt;")
                   .replace(">", "&gt;");
        ts << "  <GCPList projection=\""
           << escapedProj << "\">\n";
        const GDAL_GCP* gcps = GDALGetGCPs(srcDS);
        for (int i = 0; i < nGCPs; ++i) {
            ts << "    <GCP Id=\"" << (i + 1)
               << "\" Pixel=\"" << gcps[i].dfGCPPixel
               << "\" Line=\"" << gcps[i].dfGCPLine
               << "\" X=\"" << gcps[i].dfGCPX
               << "\" Y=\"" << gcps[i].dfGCPY
               << "\" Z=\"" << gcps[i].dfGCPZ << "\"/>\n";
        }
        ts << "  </GCPList>\n";
    } else {
        double gt[6];
        if (GDALGetGeoTransform(srcDS, gt) == CE_None) {
            ts << "  <GeoTransform>" << gt[0] << ", " << gt[1] << ", "
               << gt[2] << ", " << gt[3] << ", " << gt[4] << ", "
               << gt[5] << "</GeoTransform>\n";
        }
    }

    ts << "  <VRTRasterBand dataType=\"Float32\" band=\"1\""
          " subClass=\"VRTDerivedRasterBand\">\n";
    ts << "    <PixelFunctionType>amplitude</PixelFunctionType>\n";
    ts << "    <SimpleSource>\n";
    ts << "      <SourceFilename>" << vsiPath << "</SourceFilename>\n";
    ts << "      <SourceBand>1</SourceBand>\n";
    ts << "      <SrcDataType>" << srcTypeName << "</SrcDataType>\n";
    ts << "    </SimpleSource>\n";
    ts << "  </VRTRasterBand>\n";
    ts << "</VRTDataset>\n";

    GDALClose(srcDS);
    f.close();
    return true;
}

// Runs in background thread: for complex data, creates a VRT that wraps the
// source; for real data, extracts to a temp GeoTIFF as before.
// basePath has no extension — the function appends .vrt or .tif.
static QString processOneVsiFile(const QString& vsiPath, const QString& basePath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return QString();

    GDALRasterBandH hBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcType = GDALGetRasterDataType(hBand);
    QString result;

    if (GDALDataTypeIsComplex(srcType)) {
        QString vrtPath = basePath + ".vrt";
        if (createAmplitudeVRT(vsiPath, vrtPath))
            result = vrtPath;
    } else {
        QString tifPath = basePath + ".tif";
        GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
        if (gtDrv) {
            GDALDatasetH dstDS = GDALCreateCopy(gtDrv,
                tifPath.toUtf8().constData(), srcDS, FALSE,
                nullptr, nullptr, nullptr);
            if (dstDS) {
                int levels[] = {2, 4, 8, 16, 32, 64};
                GDALBuildOverviews(dstDS, "NEAREST", 6,
                    levels, 0, nullptr, nullptr, nullptr);
                GDALClose(dstDS);
                result = tifPath;
            }
        }
    }
    GDALClose(srcDS);
    return result;
}

void ApplicationController::wireConnections()
{
    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    connect(mWorkerManager, &WorkerManager::taskProgressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mWorkerManager, &WorkerManager::taskFinished,
        monitor, [monitor]() { monitor->onFinished(true, QString()); });
    connect(mWorkerManager, &WorkerManager::taskError,
        monitor, &ProcessingMonitorPanel::onError);

    LayerPanel* layerPanel = mMainWindow->layerPanel();
    QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();

    // 图层加载
    connect(layerPanel, &LayerPanel::layerAddRequested, this,
        [this, canvas, layerPanel, monitor](const QStringList& files) {
        if (mShuttingDown) return;
        QList<QgsMapLayer*> newLayers;

        // Phase 1: load non-VSI files directly, collect VSI entries
        struct VsiEntry { QString path; QString tmpPath; QString name; };
        QVector<VsiEntry> vsiEntries;

        for (const QString& path : files) {
            QFileInfo fi(path);
            QString name = fi.fileName();
            if (name.isEmpty() || path.startsWith("/vsi"))
                name = path.section('/', -1);

            if (path.startsWith("/vsi")) {
                QString basePath = QDir::tempPath()
                    + "/insar_" + fi.completeBaseName();
                vsiEntries.append({path, basePath, name});
            } else {
                QgsRasterLayer* layer = new QgsRasterLayer(path, name);
                if (layer->isValid()) {
                    QgsProject::instance()->addMapLayer(layer);
                    newLayers.append(layer);
                    layerPanel->onLayerLoaded(layer->id(), name,
                        QStringLiteral("Raster"));
                } else {
                    layerPanel->onLayerError(
                        QStringLiteral("无法加载: %1").arg(name));
                    delete layer;
                }
            }
        }

        auto finishLoading = [this, canvas, newLayers]() mutable {
            if (!newLayers.isEmpty()) {
                QgsMapLayer* first = newLayers.first();
                QgsCoordinateReferenceSystem crs = first->crs();
                if (crs.isValid())
                    canvas->setDestinationCrs(crs);
            }
            rebuildCanvasLayers();
            canvas->zoomToFullExtent();
        };

        if (vsiEntries.isEmpty()) {
            finishLoading();
            return;
        }

        // Phase 2: process VSI files in background thread
        int total = vsiEntries.size();
        monitor->appendLog(
            QStringLiteral("正在处理 %1 个文件...").arg(total),
            "#FF9800");

        // Build vectors for the background function (deep copies, no refs)
        QStringList vsiPaths, tmpPaths;
        QStringList names;
        for (const auto& e : vsiEntries) {
            vsiPaths.append(e.path);
            tmpPaths.append(e.tmpPath);
            names.append(e.name);
        }

        auto* watcher = new QFutureWatcher<QStringList>(this);
        connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, canvas, layerPanel, monitor, total,
             names, tmpPaths, newLayers, finishLoading]() mutable {
            const QStringList results = watcher->result();

            for (int i = 0; i < results.size(); ++i) {
                const QString& loadPath = results[i];
                if (loadPath.isEmpty()) {
                    layerPanel->onLayerError(
                        QStringLiteral("无法加载: %1").arg(names[i]));
                    continue;
                }
                mTempFiles.append(loadPath);

                QgsRasterLayer* layer =
                    new QgsRasterLayer(loadPath, names[i]);
                if (layer->isValid()) {
                    QgsProject::instance()->addMapLayer(layer);
                    newLayers.append(layer);
                    layerPanel->onLayerLoaded(layer->id(), names[i],
                        QStringLiteral("Raster"));
                    qDebug() << "[InSAR] Layer loaded:" << names[i];
                } else {
                    layerPanel->onLayerError(
                        QStringLiteral("无法加载: %1").arg(names[i]));
                    delete layer;
                    QFile::remove(loadPath);
                }
            }

            finishLoading();
            monitor->appendLog(
                QStringLiteral("文件处理完成 (%1 个图层)").arg(total),
                "#4CAF50");
            watcher->deleteLater();
        });

        watcher->setFuture(
            QtConcurrent::run([vsiPaths, tmpPaths]() -> QStringList {
                QStringList results;
                for (int i = 0; i < vsiPaths.size(); ++i)
                    results.append(
                        processOneVsiFile(vsiPaths[i], tmpPaths[i]));
                return results;
            }));
    });

    // 可见性切换
    connect(layerPanel, &LayerPanel::layerVisibilityChanged, this,
        [this](const QString& id, bool visible) {
        QgsLayerTreeLayer* node =
            QgsProject::instance()->layerTreeRoot()->findLayer(id);
        if (node) {
            node->setItemVisibilityChecked(visible);
            rebuildCanvasLayers();
        }
    });

    // 图层移除
    connect(layerPanel, &LayerPanel::layerRemoveRequested, this,
        [this](const QStringList& ids) {
        for (const QString& id : ids)
            QgsProject::instance()->removeMapLayer(id);
        rebuildCanvasLayers();
    });

    // 缩放至图层
    connect(layerPanel, &LayerPanel::zoomToLayerRequested, this,
        [canvas](const QString& id) {
        QgsMapLayer* layer = QgsProject::instance()->mapLayer(id);
        if (layer) {
            canvas->setExtent(layer->extent());
            canvas->refresh();
        }
    });
}

IRegistrationService* ApplicationController::registrationService() const
{ return mRegistrationSvc.get(); }
IInterferogramService* ApplicationController::interferogramService() const
{ return mInterferogramSvc.get(); }
IFlatEarthService* ApplicationController::flatEarthService() const
{ return mFlatEarthSvc.get(); }
IDifferentialService* ApplicationController::differentialService() const
{ return mDifferentialSvc.get(); }
IFilterService* ApplicationController::filterService() const
{ return mFilterSvc.get(); }
IUnwrappingService* ApplicationController::unwrappingService() const
{ return mUnwrappingSvc.get(); }
IGeocodingService* ApplicationController::geocodingService() const
{ return mGeocodingSvc.get(); }

void ApplicationController::rebuildCanvasLayers()
{
    QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();
    if (!canvas) return;

    QgsLayerTree* root = QgsProject::instance()->layerTreeRoot();
    QList<QgsMapLayer*> visible;
    const QStringList ids = root->findLayerIds();
    for (const QString& id : ids) {
        QgsLayerTreeLayer* node = root->findLayer(id);
        if (node && node->isVisible()) {
            QgsMapLayer* layer = node->layer();
            if (layer) visible.append(layer);
        }
    }

    canvas->setLayers(visible);
    canvas->refresh();
}

void ApplicationController::shutdown()
{
    if (mShuttingDown) return;
    mShuttingDown = true;

    qDebug() << "[InSAR] ======== 开始清理 ========";

    if (mMainWindow && mMainWindow->mapCanvasWidget()) {
        QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();
        if (canvas) {
            canvas->stopRendering();
            canvas->waitWhileRendering();
            int n = canvas->layers().size();
            canvas->setLayers(QList<QgsMapLayer*>());
            canvas->refresh();
            qDebug() << "[InSAR] 画布已清空:" << n << "个图层";
        }
    }

    QgsProject* project = QgsProject::instance();
    if (project) {
        QMap<QString, QgsMapLayer*> layers = project->mapLayers();
        int n = layers.size();
        for (auto it = layers.constBegin(); it != layers.constEnd(); ++it)
            project->removeMapLayer(it.key());
        qDebug() << "[InSAR] QgsProject 已清空:" << n << "个图层";
    }

    int tmpCount = mTempFiles.size();
    for (const QString& f : mTempFiles) {
        if (QFile::exists(f)) {
            QFile::remove(f);
            qDebug() << "[InSAR] 临时文件已删除:" << f;
        }
    }
    mTempFiles.clear();
    qDebug() << "[InSAR] 临时文件清理完成:" << tmpCount << "个";
    qDebug() << "[InSAR] ======== 清理完成 ========";
}
