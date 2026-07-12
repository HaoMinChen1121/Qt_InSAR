#include "ApplicationController.h"
#include "WorkerManager.h"
#include "mainwindow.h"
#include "ui/LayerPanel.h"
#include "ui/ProcessingMonitorPanel.h"
#include "ui/MapCanvasWidget.h"
#include "ui/SarMetadataPanel.h"

#include "services/impl/RegistrationServiceImpl.h"
#include "services/impl/InterferogramServiceImpl.h"
#include "services/impl/FlatEarthServiceImpl.h"
#include "services/impl/DifferentialServiceImpl.h"
#include "services/impl/FilterServiceImpl.h"
#include "services/impl/UnwrappingServiceImpl.h"
#include "services/impl/GeocodingServiceImpl.h"

#include "dataaccess/SarProductFactory.h"
#include "dataaccess/impl/GdalVsiProcessor.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/ISarProduct.h"

#include <qgsrasterlayer.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgsproject.h>
#include <qgsmapcanvas.h>
#include <qgsrectangle.h>
#include <qgscoordinatereferencesystem.h>

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QFileDialog>
#include <QDebug>
#include <QMessageBox>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>
#include <QScopedPointer>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

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

    connect(mMainWindow, &MainWindow::registrationRunRequested,
            this, &ApplicationController::onRegistrationRunRequested);

    connect(mMainWindow, &MainWindow::baselineEstimateRequested,
            this, &ApplicationController::onBaselineEstimateRequested);

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    connect(mWorkerManager, &WorkerManager::taskProgressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mWorkerManager, &WorkerManager::taskFinished,
        monitor, [monitor]() { monitor->onFinished(true, QString()); });
    connect(mWorkerManager, &WorkerManager::taskError,
        monitor, &ProcessingMonitorPanel::onError);

    // 配准服务进度中继
    connect(mRegistrationSvc.get(), &IProcessingService::progressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mRegistrationSvc.get(), &IProcessingService::finished, this,
        [this, monitor](bool success, const QString& outputPath) {
            if (success) {
                monitor->appendLog(
                    QStringLiteral("影像配准完成: %1").arg(outputPath),
                    "#4CAF50");
                // 加载配准结果到图层
                QStringList paths{outputPath};
                emit mMainWindow->layerPanel()->layerAddRequested(paths);
            }
            monitor->onFinished(success, outputPath);
        });
    connect(mRegistrationSvc.get(), &IProcessingService::errorOccurred,
        monitor, &ProcessingMonitorPanel::onError);

    LayerPanel* layerPanel = mMainWindow->layerPanel();
    QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();

    // 图层加载
    connect(layerPanel, &LayerPanel::layerAddRequested, this,
        [this, canvas, layerPanel, monitor](const QStringList& files) {
        if (mShuttingDown) return;
        QList<QgsMapLayer*> newLayers;

        // 捕获当前分组名（按值，避免后续产品加载时覆盖）
        QString groupName = mPendingGroupName;

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
                    layer->setCustomProperty("insar_band_path", path);
                    QgsProject::instance()->addMapLayer(layer, false);
                    if (!groupName.isEmpty()) {
                        QgsLayerTreeGroup* grp = QgsProject::instance()
                            ->layerTreeRoot()->findGroup(groupName);
                        if (grp) grp->addLayer(layer);
                        else QgsProject::instance()->layerTreeRoot()->addLayer(layer);
                    }
                    newLayers.append(layer);
                    layerPanel->onLayerLoaded(layer->id(), name,
                        QStringLiteral("Raster"), groupName);
                } else {
                    layerPanel->onLayerError(
                        QStringLiteral("无法加载: %1").arg(name));
                    delete layer;
                }
            }
        }

        auto finishLoading = [this, canvas, newLayers, groupName]() mutable {
            if (!newLayers.isEmpty()) {
                QgsMapLayer* first = newLayers.first();
                QgsCoordinateReferenceSystem crs = first->crs();
                if (crs.isValid())
                    canvas->setDestinationCrs(crs);
            }
            rebuildCanvasLayers();
            canvas->zoomToFullExtent();
            mPendingGroupName.clear();
        };

        if (vsiEntries.isEmpty()) {
            finishLoading();
            return;
        }

        int total = vsiEntries.size();
        monitor->appendLog(
            QStringLiteral("正在处理 %1 个文件...").arg(total),
            "#FF9800");

        QStringList vsiPaths, tmpPaths, names;
        for (const auto& e : vsiEntries) {
            vsiPaths.append(e.path);
            tmpPaths.append(e.tmpPath);
            names.append(e.name);
        }

        auto* watcher = new QFutureWatcher<QStringList>(this);
        connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, canvas, layerPanel, monitor, total,
             names, vsiPaths, newLayers, finishLoading, groupName]() mutable {
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
                    layer->setCustomProperty("insar_band_path", vsiPaths[i]);
                    QgsProject::instance()->addMapLayer(layer, false);
                    if (!groupName.isEmpty()) {
                        QgsLayerTreeGroup* grp = QgsProject::instance()
                            ->layerTreeRoot()->findGroup(groupName);
                        if (grp) grp->addLayer(layer);
                        else QgsProject::instance()->layerTreeRoot()->addLayer(layer);
                    }
                    newLayers.append(layer);
                    layerPanel->onLayerLoaded(layer->id(), names[i],
                        QStringLiteral("Raster"), groupName);
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
                QVector<QFuture<QString>> futures;
                futures.reserve(vsiPaths.size());
                for (int i = 0; i < vsiPaths.size(); ++i) {
                    QString path = vsiPaths[i];
                    QString tmp  = tmpPaths[i];
                    futures.append(QtConcurrent::run(
                        [path, tmp]() -> QString {
                            return GdalVsiProcessor::process(path, tmp);
                        }));
                }
                QStringList results;
                results.reserve(futures.size());
                for (auto& f : futures)
                    results.append(f.result());
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
        QgsLayerTreeGroup* root = QgsProject::instance()->layerTreeRoot();
        for (const QString& id : ids) {
            // 找到该图层所属的 QGIS 分组, 以便后续清理空分组
            QgsLayerTreeLayer* node = root->findLayer(id);
            QgsLayerTreeNode* parent = node ? node->parent() : nullptr;
            QgsProject::instance()->removeMapLayer(id);
            mSlcRegistry.remove(id);
            // 如果分组变空, 删除空分组节点
            if (parent && parent != root && parent->children().isEmpty()) {
                QgsLayerTreeNode* grandParent = parent->parent();
                if (grandParent) {
                    static_cast<QgsLayerTreeGroup*>(grandParent)
                        ->removeChildNode(parent);
                }
            }
        }
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

    // 图层选择 → 更新 SAR 元数据面板
    SarMetadataPanel* metaPanel = mMainWindow->sarMetadataPanel();
    connect(layerPanel, &LayerPanel::layerSelectionChanged, this,
        [this, metaPanel](const QString& id) {
            if (!metaPanel) return;
            auto it = mSlcRegistry.find(id);
            if (it == mSlcRegistry.end()) return;
            const SlcSourceInfo& info = it.value();
            const SarSensorInfo& s = info.sensorInfo;
            metaPanel->setMetadata(
                s.sensorType,
                s.acquisitionStart.toString("yyyy-MM-dd hh:mm"),
                sarProductTypeToString(s.productType),
                s.polarizations.join(","),
                s.wavelength,
                s.rangeSpacing,
                s.azimuthSpacing,
                s.nearRange,
                s.farRange,
                s.prf,
                s.centerFreq,
                s.orbitDirection,
                s.relativeOrbit,
                s.acquisitionMode);
        });

    // 主辅影像选择菜单连接 (在 buildSlcLayerMenu 中响应)
}

// ──────────────────────────────────────────────────────────
// Service accessors
// ──────────────────────────────────────────────────────────
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

// ──────────────────────────────────────────────────────────
// SLC 图层菜单构建
// ──────────────────────────────────────────────────────────
QMenu* ApplicationController::buildSlcLayerMenu(bool isMaster)
{
    QMenu* menu = new QMenu(mMainWindow);

    // 始终显示打开文件选项
    QAction* openAct = menu->addAction(QStringLiteral("📂 打开产品文件..."));
    connect(openAct, &QAction::triggered, this, [this, isMaster]() {
        QString path = QFileDialog::getOpenFileName(mMainWindow,
            QStringLiteral("选择 Sentinel-1 产品"),
            QString(),
            QStringLiteral("Sentinel-1 产品 (*.zip *.SAFE);;"
                           "所有文件 (*.*)"));
        if (!path.isEmpty()) {
            mPendingAutoSelect = isMaster;
            onSarProductOpenRequested(path);
        }
    });

    if (!mSlcRegistry.isEmpty()) {
        menu->addSeparator();
        for (auto it = mSlcRegistry.constBegin(); it != mSlcRegistry.constEnd(); ++it) {
            const QString& layerId = it.key();
            const SlcSourceInfo& info = it.value();
            QString label = QStringLiteral("%1 [%2] %3×%4")
                .arg(info.displayName)
                .arg(info.slcImage.polarization)
                .arg(info.slcImage.rasterSize.width())
                .arg(info.slcImage.rasterSize.height());

            QAction* act = menu->addAction(label);
            if (isMaster) {
                connect(act, &QAction::triggered, this, [this, layerId]() {
                    onMasterImageSelected(layerId);
                });
            } else {
                connect(act, &QAction::triggered, this, [this, layerId]() {
                    onSlaveImageSelected(layerId);
                });
            }
        }
    }

    return menu;
}

// ──────────────────────────────────────────────────────────
// 主影像选择
// ──────────────────────────────────────────────────────────
void ApplicationController::onMasterImageSelected(const QString& layerId)
{
    if (!mSlcRegistry.contains(layerId)) return;

    mSelectedMasterLayerId = layerId;
    const SlcSourceInfo& info = mSlcRegistry[layerId];

    mMainWindow->regParams().masterPath = info.slcImage.filePath;
    mMainWindow->regParams().masterSlcBandPath = info.bandPath;
    mMainWindow->regParams().masterOrbitVectors = info.orbitVectors;
    mMainWindow->regParams().masterDoppler = info.doppler;
    mMainWindow->regParams().masterRangeSpacing = info.sensorInfo.rangeSpacing;
    mMainWindow->regParams().masterAzimuthSpacing = info.sensorInfo.azimuthSpacing;
    mMainWindow->regParams().masterNearRange = info.sensorInfo.nearRange;
    mMainWindow->regParams().masterPrf = info.sensorInfo.prf;
    mMainWindow->regParams().wavelength = info.sensorInfo.wavelength;

    mMainWindow->updateImageSelectionLabel(
        mMainWindow->masterInfoLabel(), info.displayName);

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    if (monitor) {
        monitor->appendLog(
            QStringLiteral("主影像: %1 %2 %3×%4 轨道号:%5")
                .arg(info.displayName)
                .arg(info.slcImage.polarization)
                .arg(info.slcImage.rasterSize.width())
                .arg(info.slcImage.rasterSize.height())
                .arg(info.sensorInfo.relativeOrbit),
            "#4A90D9");
    }
}

// ──────────────────────────────────────────────────────────
// 辅影像选择
// ──────────────────────────────────────────────────────────
void ApplicationController::onSlaveImageSelected(const QString& layerId)
{
    if (!mSlcRegistry.contains(layerId)) return;

    mSelectedSlaveLayerId = layerId;
    const SlcSourceInfo& info = mSlcRegistry[layerId];

    mMainWindow->regParams().slavePath = info.slcImage.filePath;
    mMainWindow->regParams().slaveSlcBandPath = info.bandPath;
    mMainWindow->regParams().slaveOrbitVectors = info.orbitVectors;
    mMainWindow->regParams().slaveDoppler = info.doppler;
    mMainWindow->regParams().wavelength = info.sensorInfo.wavelength;

    mMainWindow->updateImageSelectionLabel(
        mMainWindow->slaveInfoLabel(), info.displayName);

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    if (monitor) {
        monitor->appendLog(
            QStringLiteral("辅影像: %1 %2 %3×%4 轨道号:%5")
                .arg(info.displayName)
                .arg(info.slcImage.polarization)
                .arg(info.slcImage.rasterSize.width())
                .arg(info.slcImage.rasterSize.height())
                .arg(info.sensorInfo.relativeOrbit),
            "#4A90D9");
    }

    // 配对预检
    if (!mSelectedMasterLayerId.isEmpty()) {
        const SlcSourceInfo& mInfo = mSlcRegistry[mSelectedMasterLayerId];
        if (info.sensorInfo.relativeOrbit != mInfo.sensorInfo.relativeOrbit) {
            monitor->appendLog(
                QStringLiteral("警告: 主辅影像轨道号不同 (%1 vs %2), 可能不是同一轨道!")
                    .arg(mInfo.sensorInfo.relativeOrbit)
                    .arg(info.sensorInfo.relativeOrbit),
                "#E67E22");
        }
        if (info.slcImage.polarization != mInfo.slcImage.polarization) {
            monitor->appendLog(
                QStringLiteral("警告: 主辅影像极化方式不同 (%1 vs %2)")
                    .arg(mInfo.slcImage.polarization)
                    .arg(info.slcImage.polarization),
                "#E67E22");
        }
    }
}

// ──────────────────────────────────────────────────────────
// 配准执行
// ──────────────────────────────────────────────────────────
void ApplicationController::onRegistrationRunRequested(
    const RegistrationParams& params)
{
    mRegistrationSvc->setParams(params);
    mWorkerManager->enqueueTask(mRegistrationSvc.get());
}

// ──────────────────────────────────────────────────────────
// 基线快速估算
// ──────────────────────────────────────────────────────────
void ApplicationController::onBaselineEstimateRequested(
    const QString& masterPath, const QString& slavePath)
{
    Q_UNUSED(masterPath);
    Q_UNUSED(slavePath);

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();

    if (mSelectedMasterLayerId.isEmpty() || mSelectedSlaveLayerId.isEmpty()) {
        monitor->appendLog(
            QStringLiteral("基线估算失败: 主辅影像未选择"), "#E74C3C");
        return;
    }

    const SlcSourceInfo& mInfo = mSlcRegistry[mSelectedMasterLayerId];
    const SlcSourceInfo& sInfo = mSlcRegistry[mSelectedSlaveLayerId];

    if (mInfo.orbitVectors.size() < 2 || sInfo.orbitVectors.size() < 2) {
        monitor->appendLog(
            QStringLiteral("基线估算失败: 轨道数据不足"), "#E74C3C");
        return;
    }

    // 临时调用 RegistrationServiceImpl 的基线估算
    // 通过设置 params 后调用内部方法不够直接，这里直接计算
    // 取主影像中间时刻
    double t0 = (mInfo.orbitVectors.first().time
        + mInfo.orbitVectors.last().time) * 0.5;

    // 三次样条插值到 t0
    // 此处直接复用 RegistrationServiceImpl 的基线估算逻辑
    // 为简单，直接构造 RegistrationParams 并触发服务中的逻辑

    mMainWindow->regParams().masterOrbitVectors = mInfo.orbitVectors;
    mMainWindow->regParams().slaveOrbitVectors  = sInfo.orbitVectors;
    mMainWindow->regParams().masterDoppler = mInfo.doppler;
    mMainWindow->regParams().slaveDoppler  = sInfo.doppler;
    mMainWindow->regParams().masterNearRange = mInfo.sensorInfo.nearRange;
    mMainWindow->regParams().masterRangeSpacing = mInfo.sensorInfo.rangeSpacing;
    mMainWindow->regParams().wavelength = mInfo.sensorInfo.wavelength;

    RegistrationParams p = mMainWindow->collectRegParams();
    p.masterSlcBandPath = mInfo.bandPath;
    p.slaveSlcBandPath  = sInfo.bandPath;
    p.masterOrbitVectors = mInfo.orbitVectors;
    p.slaveOrbitVectors  = sInfo.orbitVectors;
    p.masterDoppler = mInfo.doppler;
    p.slaveDoppler  = sInfo.doppler;
    p.masterNearRange = mInfo.sensorInfo.nearRange;
    p.masterRangeSpacing = mInfo.sensorInfo.rangeSpacing;
    p.wavelength = mInfo.sensorInfo.wavelength;
    p.estimateBaseline = true;
    // 不做完整配准 — 通过设置粗配准方法为 orbit (只做轨道法) 并最小化操作
    // 因为 RegistrationServiceImpl 的基线估算嵌入在 execute 中
    // 我们直接执行完整的配准链，但在早期就输出基线信息

    // 更简洁的方式: 直接运行完整配准，日志中会输出基线信息
    monitor->appendLog(
        QStringLiteral("基线估算: 正在计算..."), "#4A90D9");

    mRegistrationSvc->setParams(p);
    mWorkerManager->enqueueTask(mRegistrationSvc.get());
}

// ──────────────────────────────────────────────────────────
// SAR 产品打开
// ──────────────────────────────────────────────────────────
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

    SarSensorInfo sensorInfo = product->sensorInfo();
    QList<OrbitStateVector> orbitVectors = product->orbitStateVectors();
    DopplerInfo doppler = product->dopplerCentroid();
    QString productPath = product->originalPath();

    const auto& bands = product->bands();
    LayerPanel* layerPanel = mMainWindow->layerPanel();
    if (layerPanel && !bands.isEmpty()) {
        // 构建产品分组名
        QString groupName = QStringLiteral("%1 %2 %3 Orbit%4")
            .arg(sensorInfo.missionId.isEmpty() ? sensorInfo.sensorType : sensorInfo.missionId)
            .arg(sensorInfo.acquisitionMode)
            .arg(sensorInfo.acquisitionStart.toString("yyyy-MM-dd"))
            .arg(sensorInfo.relativeOrbit);
        mPendingGroupName = groupName;

        // 在 QGIS 图层树中创建分组
        QgsLayerTreeGroup* qgisGroup =
            QgsProject::instance()->layerTreeRoot()->addGroup(groupName);

        QStringList paths;
        for (const auto& b : bands) {
            paths.append(b.rasterPath);

            // 构建 SlcSourceInfo
            SlcSourceInfo srcInfo;
            srcInfo.productPath = productPath;
            srcInfo.bandPath = b.rasterPath;
            srcInfo.bandIndex = b.index;
            srcInfo.displayName = QStringLiteral("%1_%2")
                .arg(b.subSwath).arg(b.polarization);
            srcInfo.sensorInfo = sensorInfo;
            srcInfo.orbitVectors = orbitVectors;
            srcInfo.doppler = doppler;

            // 填充基础 SlcImage
            srcInfo.slcImage.filePath = b.rasterPath;
            srcInfo.slcImage.rasterSize = b.rasterSize;
            srcInfo.slcImage.polarization = b.polarization;
            srcInfo.slcImage.wavelength = sensorInfo.wavelength;
            srcInfo.slcImage.rangeSpacing = sensorInfo.rangeSpacing;
            srcInfo.slcImage.azimuthSpacing = sensorInfo.azimuthSpacing;
            srcInfo.slcImage.nearRange = sensorInfo.nearRange;
            srcInfo.slcImage.farRange = sensorInfo.farRange;
            srcInfo.slcImage.prf = sensorInfo.prf;
            srcInfo.slcImage.centerFreq = sensorInfo.centerFreq;

            // 暂存，等图层 ID 分配后再关联
            mPendingSlcRegistry[b.rasterPath] = srcInfo;
        }
        emit layerPanel->layerAddRequested(paths);
    }

    SarMetadataPanel* metaPanel = mMainWindow->sarMetadataPanel();
    if (metaPanel) {
        metaPanel->setMetadata(
            sensorInfo.sensorType,
            sensorInfo.acquisitionStart.toString("yyyy-MM-dd hh:mm"),
            sarProductTypeToString(sensorInfo.productType),
            sensorInfo.polarizations.join(","),
            sensorInfo.wavelength,
            sensorInfo.rangeSpacing,
            sensorInfo.azimuthSpacing,
            sensorInfo.nearRange,
            sensorInfo.farRange,
            sensorInfo.prf,
            sensorInfo.centerFreq,
            sensorInfo.orbitDirection,
            sensorInfo.relativeOrbit,
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

    // 反转：树顶部 = 画布顶层（用户直觉）而非QGIS默认的反向
    std::reverse(visible.begin(), visible.end());
    canvas->setLayers(visible);
    canvas->refresh();

    // 关联 SLC 注册表: VSI 图层加载后会分配 QGIS layer ID
    // 在此处将 mPendingSlcRegistry 中的条目迁移到 mSlcRegistry
    if (!mPendingSlcRegistry.isEmpty()) {
        QMap<QString, QgsMapLayer*> layers = QgsProject::instance()->mapLayers();
        QString firstNewId;
        for (auto it = layers.constBegin(); it != layers.constEnd(); ++it) {
            const QString& layerId = it.key();
            QgsMapLayer* layer = it.value();
            if (!layer) continue;
            QString bandPath = layer->customProperty("insar_band_path").toString();
            if (!bandPath.isEmpty() && mPendingSlcRegistry.contains(bandPath)) {
                mSlcRegistry[layerId] = mPendingSlcRegistry.take(bandPath);
                if (firstNewId.isEmpty()) firstNewId = layerId;
            }
        }
        mPendingSlcRegistry.clear();

        // 自动选择 (从"打开产品文件"菜单触发)
        if (mPendingAutoSelect >= 0 && !firstNewId.isEmpty()) {
            if (mPendingAutoSelect == 1)
                onMasterImageSelected(firstNewId);
            else
                onSlaveImageSelected(firstNewId);
            mPendingAutoSelect = -1;
        }
    }

    // 更新主辅影像选择按钮的菜单
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

    mSlcRegistry.clear();
    mPendingSlcRegistry.clear();

    qDebug() << "[InSAR] 临时文件清理完成:" << tmpCount << "个";
    qDebug() << "[InSAR] ======== 清理完成 ========";
}
