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
#include <cmath>
#include <vector>
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

static bool convertComplexToAmplitude(GDALDatasetH srcDS, const QString& dstPath)
{
    int w = GDALGetRasterXSize(srcDS);
    int h = GDALGetRasterYSize(srcDS);
    GDALRasterBandH srcBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcType = GDALGetRasterDataType(srcBand);

    if (!GDALDataTypeIsComplex(srcType))
        return false;

    GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
    if (!gtDrv) return false;

    GDALDatasetH dstDS = GDALCreate(gtDrv, dstPath.toUtf8().constData(),
                                     w, h, 1, GDT_Float32, nullptr);
    if (!dstDS) return false;

    double geoTransform[6];
    if (GDALGetGeoTransform(srcDS, geoTransform) == CE_None) {
        GDALSetGeoTransform(dstDS, geoTransform);
    } else {
        int nGCPs = GDALGetGCPCount(srcDS);
        if (nGCPs > 0)
            GDALSetGCPs(dstDS, nGCPs, GDALGetGCPs(srcDS),
                         GDALGetGCPProjection(srcDS));
    }
    GDALSetProjection(dstDS, GDALGetProjectionRef(srcDS));

    int elemSize = GDALGetDataTypeSizeBytes(srcType);
    std::vector<char> srcBuf(static_cast<size_t>(w) * elemSize);
    std::vector<float> dstBuf(w);

    for (int y = 0; y < h; y++) {
        if (GDALRasterIO(srcBand, GF_Read, 0, y, w, 1,
                         srcBuf.data(), w, 1, srcType, 0, 0) != CE_None)
            break;

        if (srcType == GDT_CInt16) {
            const auto* p = reinterpret_cast<const int16_t*>(srcBuf.data());
            for (int x = 0; x < w; x++) {
                float I = static_cast<float>(p[x * 2]);
                float Q = static_cast<float>(p[x * 2 + 1]);
                dstBuf[x] = std::sqrt(I * I + Q * Q);
            }
        } else if (srcType == GDT_CInt32) {
            const auto* p = reinterpret_cast<const int32_t*>(srcBuf.data());
            for (int x = 0; x < w; x++) {
                float I = static_cast<float>(p[x * 2]);
                float Q = static_cast<float>(p[x * 2 + 1]);
                dstBuf[x] = std::sqrt(I * I + Q * Q);
            }
        } else if (srcType == GDT_CFloat32) {
            const auto* p = reinterpret_cast<const float*>(srcBuf.data());
            for (int x = 0; x < w; x++) {
                float I = p[x * 2];
                float Q = p[x * 2 + 1];
                dstBuf[x] = std::sqrt(I * I + Q * Q);
            }
        } else {
            GDALClose(dstDS);
            return false;
        }

        GDALRasterIO(GDALGetRasterBand(dstDS, 1), GF_Write, 0, y, w, 1,
                     dstBuf.data(), w, 1, GDT_Float32, 0, 0);
    }

    int levels[] = {2, 4, 8, 16, 32, 64};
    GDALBuildOverviews(dstDS, "NEAREST", 6, levels, 0,
                       nullptr, nullptr, nullptr);
    GDALClose(dstDS);
    return true;
}

// Runs in background thread: opens VSI path and extracts/converts to temp TIFF.
// Returns the temp path on success, empty QString on failure.
static QString processOneVsiFile(const QString& vsiPath, const QString& tmpPath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return QString();

    GDALRasterBandH hBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcType = GDALGetRasterDataType(hBand);
    QString result;

    if (GDALDataTypeIsComplex(srcType)) {
        if (convertComplexToAmplitude(srcDS, tmpPath))
            result = tmpPath;
    } else {
        GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
        if (gtDrv) {
            GDALDatasetH dstDS = GDALCreateCopy(gtDrv,
                tmpPath.toUtf8().constData(), srcDS, FALSE,
                nullptr, nullptr, nullptr);
            if (dstDS) {
                int levels[] = {2, 4, 8, 16, 32, 64};
                GDALBuildOverviews(dstDS, "NEAREST", 6,
                    levels, 0, nullptr, nullptr, nullptr);
                GDALClose(dstDS);
                result = tmpPath;
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
                QString tmpPath = QDir::tempPath()
                    + "/insar_" + fi.completeBaseName() + ".tif";
                vsiEntries.append({path, tmpPath, name});
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
