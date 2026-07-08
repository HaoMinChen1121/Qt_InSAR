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
#include <qgslayertreemapcanvasbridge.h>
#include <qgsproject.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDebug>
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

    // 图层树 ↔ 画布桥接
    QgsLayerTree* layerTree = QgsProject::instance()->layerTreeRoot();
    mLayerTreeBridge = new QgsLayerTreeMapCanvasBridge(layerTree, canvas, this);

    // 图层加载
    connect(layerPanel, &LayerPanel::layerAddRequested, this,
        [this, canvas, layerPanel](const QStringList& files) {
        if (mShuttingDown) return;
        QList<QgsMapLayer*> newLayers;

        for (const QString& path : files) {
            QFileInfo fi(path);
            QString name = fi.fileName();
            if (name.isEmpty() || path.startsWith("/vsi"))
                name = path.section('/', -1);

            QString loadPath = path;

            if (path.startsWith("/vsi")) {
                GDALDatasetH srcDS = GDALOpen(
                    path.toUtf8().constData(), GA_ReadOnly);
                if (srcDS) {
                    QString tmpPath = QDir::tempPath()
                        + "/insar_" + fi.completeBaseName() + ".tif";
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
                            loadPath = tmpPath;
                            mTempFiles.append(tmpPath);
                            qDebug() << "[InSAR] TIFF extracted:"
                                     << tmpPath;
                        }
                    }
                    GDALClose(srcDS);
                }
            }

            QgsRasterLayer* layer = new QgsRasterLayer(loadPath, name);
            if (layer->isValid()) {
                QgsProject::instance()->addMapLayer(layer);
                newLayers.append(layer);
                layerPanel->onLayerLoaded(layer->id(), name,
                    QStringLiteral("Raster"));
                qDebug() << "[InSAR] Layer loaded:" << name;
            } else {
                layerPanel->onLayerError(
                    QStringLiteral("无法加载: %1").arg(name));
                delete layer;
                if (loadPath != path && QFile::exists(loadPath)) {
                    QFile::remove(loadPath);
                    mTempFiles.removeAll(loadPath);
                }
            }
        }

        if (!newLayers.isEmpty()) {
            QgsMapLayer* first = newLayers.first();
            QgsCoordinateReferenceSystem layerCrs = first->crs();
            if (layerCrs.isValid()) {
                canvas->setDestinationCrs(layerCrs);
            }
            canvas->zoomToFullExtent();
            qDebug() << "[InSAR] Canvas ready, layers:" << newLayers.size();
        }
    });

    // 可见性切换
    connect(layerPanel, &LayerPanel::layerVisibilityChanged, this,
        [](const QString& id, bool visible) {
        QgsLayerTreeLayer* node =
            QgsProject::instance()->layerTreeRoot()->findLayer(id);
        if (node) {
            node->setItemVisibilityChecked(visible);
        }
    });

    // 图层移除
    connect(layerPanel, &LayerPanel::layerRemoveRequested, this,
        [](const QStringList& ids) {
        for (const QString& id : ids)
            QgsProject::instance()->removeMapLayer(id);
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

void ApplicationController::shutdown()
{
    if (mShuttingDown) return;
    mShuttingDown = true;

    qDebug() << "[InSAR] ======== 开始清理 ========";

    if (mLayerTreeBridge) {
        delete mLayerTreeBridge;
        mLayerTreeBridge = nullptr;
    }
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
