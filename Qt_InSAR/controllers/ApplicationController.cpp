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
#include <QMessageBox>
#include <QScopedPointer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include "dataaccess/impl/GdalVsiProcessor.h"
#include "dataaccess/SarProductFactory.h"
#include "ui/SarMetadataPanel.h"

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
    GdalVsiProcessor::registerPixelFunctions();
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
    connect(mMainWindow, &MainWindow::sarProductOpenRequested,
            this, &ApplicationController::onSarProductOpenRequested);

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
                        GdalVsiProcessor::process(vsiPaths[i], tmpPaths[i]));
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

void ApplicationController::onSarProductOpenRequested(const QString& path)
{
    if (mShuttingDown) return;

    QScopedPointer<ISarProduct> product(createSarProduct(path));
    if (!product || !product->open(path)) {
        QMessageBox::warning(mMainWindow,
            QStringLiteral("打开失败"),
            QStringLiteral("无法识别该 Sentinel-1 产品。\n"
                           "请确认选择的是 .SAFE 目录或 .zip 文件。"));
        return;
    }

    const auto& bands = product->bands();
    LayerPanel* layerPanel = mMainWindow->layerPanel();
    if (layerPanel && !bands.isEmpty()) {
        QStringList paths;
        for (const auto& b : bands)
            paths.append(b.rasterPath);
        emit layerPanel->layerAddRequested(paths);
    }

    SarSensorInfo info = product->sensorInfo();
    SarMetadataPanel* metaPanel = mMainWindow->sarMetadataPanel();
    if (metaPanel) {
        metaPanel->setMetadata(
            info.sensorType,
            info.acquisitionStart.toString("yyyy-MM-dd hh:mm"),
            sarProductTypeToString(info.productType),
            info.polarizations.join(","),
            info.wavelength,
            info.rangeSpacing,
            info.azimuthSpacing,
            info.nearRange,
            info.farRange,
            info.prf,
            info.centerFreq,
            info.orbitDirection,
            info.relativeOrbit,
            product->acquisitionMode()
        );
    }

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    if (monitor) {
        QStringList bandInfo;
        for (const auto& b : bands)
            bandInfo.append(QStringLiteral("  %1 %2 %3×%4 %5")
                .arg(b.polarization)
                .arg(b.subSwath)
                .arg(b.rasterSize.width())
                .arg(b.rasterSize.height())
                .arg(b.dataType));

        monitor->appendLog(
            QStringLiteral("已加载 Sentinel-1 产品: %1 (%2)\n波段:\n%3")
                .arg(product->productId())
                .arg(sarProductTypeToString(product->productType()))
                .arg(bandInfo.join("\n")),
            "#4CAF50");
    }
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
