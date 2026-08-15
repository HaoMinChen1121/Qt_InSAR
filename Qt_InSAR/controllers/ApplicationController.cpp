#include "ApplicationController.h"
#include "WorkerManager.h"
#include "mainwindow.h"
#include "ui/LayerPanel.h"
#include "ui/ProcessingMonitorPanel.h"
#include "ui/MapCanvasWidget.h"
#include "ui/SarMetadataPanel.h"

#include "services/registration/RegistrationServiceImpl.h"
#include "services/registration/strategy/ProductDetector.h"
#include "services/interferogram/InterferogramServiceImpl.h"
#include "services/impl/FilterServiceImpl.h"
#include "services/impl/UnwrappingServiceImpl.h"
#include "services/impl/GeocodingServiceImpl.h"

#include "dataaccess/SarProductFactory.h"
#include "renderers/RasterRenderer.h"
#include "renderers/Sentinel1RasterProvider.h"
#include "renderers/InSarTiffRasterProvider.h"
#include "dataaccess/impl/SentinelZipProduct.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/QsarIO.h"
#include "dataaccess/ISarProduct.h"

#include <gdal_priv.h>

#include <qgsrasterlayer.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgsproviderregistry.h>

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
#include <QApplication>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>

namespace {
// "/vsizip/F:/data/x.zip/...SAFE/measurement/iw1-vv.tiff" → sentinel1zip URI
QString toSentinel1Uri(const QString& path)
{
    QString real = path;
    if (real.startsWith("/vsizip/", Qt::CaseInsensitive))
        real = real.mid(8);
    else if (real.startsWith("/vsizip", Qt::CaseInsensitive))
        real = real.mid(7);
    else
        return QString();
    int zipPos = real.indexOf(".zip/", 0, Qt::CaseInsensitive);
    if (zipPos < 0) return QString();
    QString zipPath = real.left(zipPos + 4);
    QString entry = real.mid(zipPos + 5);
    return Sentinel1RasterProvider::buildUri(zipPath, entry);
}

// 本项目输出的复数 GeoTIFF (单波段 CFloat32) → insartiff URI
QString toInSarTiffUri(const QString& path)
{
    GDALDatasetH hDS = GDALOpenEx(path.toUtf8().constData(),
        GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!hDS) return QString();
    bool ours = (GDALGetRasterCount(hDS) == 1)
        && (GDALGetRasterDataType(GDALGetRasterBand(hDS, 1)) == GDT_CFloat32);
    GDALClose(hDS);
    return ours ? InSarTiffRasterProvider::buildUri(path) : QString();
}
} // namespace

ApplicationController::ApplicationController(MainWindow* mainWindow, QObject* parent)
    : QObject(parent), mMainWindow(mainWindow)
{
    mWorkerManager = new WorkerManager(this);
    createServices();
}

ApplicationController::~ApplicationController() { shutdown(); }

void ApplicationController::initialize()
{
    // 注册自定义栅格 Provider: Sentinel-1 ZIP 直接读取 + 本项目输出 GeoTIFF
    QgsProviderRegistry::instance()->registerProvider(
        new Sentinel1ProviderMetadata());
    QgsProviderRegistry::instance()->registerProvider(
        new InSarTiffProviderMetadata());
    wireConnections();
}

void ApplicationController::createServices()
{
    mRegistrationSvc = std::make_unique<RegistrationServiceImpl>(this);
    mInterferogramSvc = std::make_unique<InterferogramServiceImpl>(this);
    mFilterSvc = std::make_unique<FilterServiceImpl>(this);
    mUnwrappingSvc = std::make_unique<UnwrappingServiceImpl>(this);
    mGeocodingSvc = std::make_unique<GeocodingServiceImpl>(this);
    // Product 驱动: 滤波按 productId 经注册表解析输入
    static_cast<FilterServiceImpl*>(mFilterSvc.get())->setProductManager(&mProductManager);
}

void ApplicationController::wireConnections()
{
    connect(mMainWindow, &MainWindow::sarProductOpenRequested,
            this, &ApplicationController::onSarProductOpenRequested);
    connect(mMainWindow, &MainWindow::registrationRunRequested,
            this, &ApplicationController::onRegistrationRunRequested);
    connect(mMainWindow, &MainWindow::baselineEstimateRequested,
            this, &ApplicationController::onBaselineEstimateRequested);
    connect(mMainWindow, &MainWindow::interferogramRunRequested,
            this, &ApplicationController::onInterferogramRunRequested);

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    connect(mWorkerManager, &WorkerManager::taskProgressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mWorkerManager, &WorkerManager::taskFinished,
        monitor, [monitor]() { monitor->onFinished(true, QString()); });
    connect(mWorkerManager, &WorkerManager::taskError,
        monitor, &ProcessingMonitorPanel::onError);

    connect(mRegistrationSvc.get(), &IProcessingService::progressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mRegistrationSvc.get(), &IProcessingService::finished, this,
        [this, monitor](bool success, const QString& outputPath) {
            if (success) {
                monitor->appendLog(
                    QStringLiteral("影像配准完成: %1").arg(outputPath), "#4CAF50");
                if (outputPath.endsWith(".qsar", Qt::CaseInsensitive))
                    mProductManager.registerProduct(outputPath);
                emit mMainWindow->layerPanel()->layerAddRequested({outputPath});
            }
            monitor->onFinished(success, outputPath);
        });
    connect(mRegistrationSvc.get(), &IProcessingService::errorOccurred,
        monitor, &ProcessingMonitorPanel::onError);

    connect(mInterferogramSvc.get(), &IProcessingService::progressChanged,
        monitor, &ProcessingMonitorPanel::onProgress);
    connect(mInterferogramSvc.get(), &IProcessingService::finished, this,
        [this, monitor](bool success, const QString& outputPath) {
            if (success) {
                monitor->appendLog(QStringLiteral("干涉图生成完成: %1").arg(outputPath), "#4CAF50");
                if (outputPath.endsWith(".qsar", Qt::CaseInsensitive))
                    mProductManager.registerProduct(outputPath);
                // autoLoadToCanvas=false 时只写 QSAR, 由用户手动加载
                if (mInterferogramSvc->params().autoLoadToCanvas)
                    emit mMainWindow->layerPanel()->layerAddRequested({outputPath});
            }
            monitor->onFinished(success, outputPath);
        });
    connect(mInterferogramSvc.get(), &IProcessingService::errorOccurred,
        monitor, &ProcessingMonitorPanel::onError);

    LayerPanel* layerPanel = mMainWindow->layerPanel();
    QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();

    // 图层加载
    connect(layerPanel, &LayerPanel::layerAddRequested, this,
        [this, canvas, layerPanel, monitor](const QStringList& files) {
        if (mShuttingDown) return;
        QString groupName = mPendingGroupName;

        // 展开 .qsar 产品文件
        QStringList expandedFiles;
        for (const QString& f : files) {
            if (f.endsWith(".qsar", Qt::CaseInsensitive)) {
                QsarProduct qsar = QsarIO::read(f);
                if (!qsar.bands.isEmpty()) {
                    // 设置分组名
                    mPendingGroupName = qsar.productType + " " + qsar.sourceMaster;
                    groupName = mPendingGroupName;
                    QgsProject::instance()->layerTreeRoot()->insertGroup(0, groupName);
                    // 检查是否有任何 band 标记为 visible (向后兼容旧 .qsar)
                    bool hasVisible = false;
                    for (const auto& b : qsar.bands)
                        if (b.defaultVisible) { hasVisible = true; break; }
                    for (const auto& b : qsar.bands) {
                        if (hasVisible && !b.defaultVisible) continue;
                        expandedFiles.append(b.file);
                    }
                    monitor->appendLog(
                        QStringLiteral("加载QSAR产品: %1 (%2波段)")
                            .arg(QFileInfo(f).fileName()).arg(qsar.bands.size()),
                        "#4CAF50");
                }
            } else {
                expandedFiles.append(f);
            }
        }

        QList<QgsMapLayer*> newLayers;
        for (const QString& path : expandedFiles) {
            QFileInfo fi(path);
            QString name = fi.fileName();
            if (name.isEmpty() || path.startsWith("/vsi"))
                name = path.section('/', -1);

            // /vsizip/ 路径 → 自定义 sentinel1zip Provider (ZIP 内直接渲染)
            // 本项目输出 GeoTIFF → insartiff Provider (幅度渲染+快速采样统计)
            // 转换失败则保持原路径交给 GDAL
            QString uri = path;
            QString providerKey;
            if (path.startsWith("/vsi", Qt::CaseInsensitive)) {
                QString converted = toSentinel1Uri(path);
                if (!converted.isEmpty()) {
                    uri = converted;
                    providerKey = QStringLiteral("sentinel1zip");
                }
            } else {
                QString converted = toInSarTiffUri(path);
                if (!converted.isEmpty()) {
                    uri = converted;
                    providerKey = QStringLiteral("insartiff");
                }
            }

            QgsRasterLayer* layer = providerKey.isEmpty()
                ? new QgsRasterLayer(uri, name)
                : new QgsRasterLayer(uri, name, providerKey);
            if (layer->isValid()) {
                layer->setCustomProperty("insar_band_path", uri);
                if (!groupName.isEmpty()) {
                    // SAR 产品: 加到项目+分组
                    QgsProject::instance()->addMapLayer(layer, false);
                    QgsLayerTreeGroup* grp = QgsProject::instance()
                        ->layerTreeRoot()->findGroup(groupName);
                    if (grp) grp->insertLayer(0, layer);
                    else QgsProject::instance()->layerTreeRoot()->insertLayer(0, layer);
                } else {
                    // 通用栅格: QGIS 自动管理图层树
                    QgsProject::instance()->addMapLayer(layer);
                }
                newLayers.append(layer);
                RasterRenderer::applyAutoRenderer(layer, name);
                QString layerType = QStringLiteral("Raster");
                if (name.contains("_phase")) layerType = QStringLiteral("相位");
                else if (name.contains("_coh")) layerType = QStringLiteral("相干性");
                layerPanel->onLayerLoaded(layer->id(), name,
                    layerType, groupName);
                qDebug() << "[InSAR] Layer loaded:" << name;
            } else {
                layerPanel->onLayerError(QStringLiteral("无法加载: %1").arg(name));
                delete layer;
            }
        }

        if (!newLayers.isEmpty()) {
            QgsMapLayer* first = newLayers.first();
            QgsCoordinateReferenceSystem crs = first->crs();
            canvas->setDestinationCrs(crs);
        }
        rebuildCanvasLayers();
        canvas->zoomToFullExtent();
        mPendingGroupName.clear();
        if (mPendingLoadCount > 0) --mPendingLoadCount;
    });

    connect(layerPanel, &LayerPanel::layerVisibilityChanged, this,
        [this](const QString& id, bool visible) {
        QgsLayerTreeLayer* node = QgsProject::instance()->layerTreeRoot()->findLayer(id);
        if (node) {
            node->setItemVisibilityChecked(visible);
        } else {
            QgsMapLayer* layer = QgsProject::instance()->mapLayer(id);
            if (layer) {
                QgsProject::instance()->layerTreeRoot()->insertLayer(0, layer);
                qWarning() << "[Layer] 图层在注册表但不在树中, 已恢复:" << layer->name();
            }
        }
        rebuildCanvasLayers();
    });

    connect(layerPanel, &LayerPanel::layerRemoveRequested, this,
        [this, layerPanel](const QStringList& ids) {
        QgsLayerTreeGroup* root = QgsProject::instance()->layerTreeRoot();
        for (const QString& id : ids) {
            QgsLayerTreeLayer* node = root->findLayer(id);
            QgsLayerTreeNode* parent = node ? node->parent() : nullptr;
            QgsProject::instance()->removeMapLayer(id);
            layerPanel->onLayerRemoved(id);  // 同步移除 QTreeWidget 项
            if (parent && parent != root && parent->children().isEmpty()) {
                QgsLayerTreeNode* gp = parent->parent();
                if (gp) static_cast<QgsLayerTreeGroup*>(gp)->removeChildNode(parent);
            }
        }
        rebuildCanvasLayers();
    });

    connect(layerPanel, &LayerPanel::fullExtentRequested, this,
        [canvas]() {
            canvas->zoomToFullExtent();
        });

    connect(layerPanel, &LayerPanel::zoomToLayerRequested, this,
        [canvas](const QString& id) {
        QgsMapLayer* layer = QgsProject::instance()->mapLayer(id);
        if (layer) { canvas->setExtent(layer->extent()); canvas->refresh(); }
    });

    SarMetadataPanel* metaPanel = mMainWindow->sarMetadataPanel();
    connect(layerPanel, &LayerPanel::layerSelectionChanged, this,
        [this, metaPanel](const QString& /*id*/) {
            if (!metaPanel || mSelectedMasterPath.isEmpty()) return;
            auto it = mProductRegistry.find(mSelectedMasterPath);
            if (it == mProductRegistry.end()) return;
            const SarSensorInfo& s = it->sensorInfo;
            metaPanel->setMetadata(s.sensorType,
                s.acquisitionStart.toString("yyyy-MM-dd hh:mm"),
                sarProductTypeToString(s.productType), s.polarizations.join(","),
                s.wavelength, s.rangeSpacing, s.azimuthSpacing,
                s.nearRange, s.farRange, s.prf, s.centerFreq,
                s.orbitDirection, s.relativeOrbit, s.acquisitionMode);
        });
}

IRegistrationService* ApplicationController::registrationService() const
{ return mRegistrationSvc.get(); }
IInterferogramService* ApplicationController::interferogramService() const
{ return mInterferogramSvc.get(); }
IFilterService* ApplicationController::filterService() const
{ return mFilterSvc.get(); }
IUnwrappingService* ApplicationController::unwrappingService() const
{ return mUnwrappingSvc.get(); }
IGeocodingService* ApplicationController::geocodingService() const
{ return mGeocodingSvc.get(); }

// ──────────────────────────────────────────────────────────
// 产品菜单
// ──────────────────────────────────────────────────────────
QMenu* ApplicationController::buildProductMenu(bool isMaster)
{
    QMenu* menu = new QMenu(mMainWindow);

    QAction* openAct = menu->addAction(QStringLiteral("📂 打开产品文件..."));
    connect(openAct, &QAction::triggered, this, [this, isMaster]() {
        QString path = QFileDialog::getOpenFileName(mMainWindow,
            QStringLiteral("选择 Sentinel-1 产品"), QString(),
            QStringLiteral("Sentinel-1 产品 (*.zip *.SAFE);;所有文件 (*.*)"));
        if (!path.isEmpty()) {
            mPendingAutoSelect = isMaster ? 1 : 0;
            onSarProductOpenRequested(path);
        }
    });

    if (!mProductRegistry.isEmpty()) {
        menu->addSeparator();
        for (auto it = mProductRegistry.constBegin(); it != mProductRegistry.constEnd(); ++it) {
            const QString& path = it.key();
            const ProductSourceInfo& info = it.value();
            QString label = QStringLiteral("%1 [%2bands] Orbit%3")
                .arg(info.displayName)
                .arg(info.bands.size())
                .arg(info.sensorInfo.relativeOrbit);
            QAction* act = menu->addAction(label);
            if (isMaster) {
                connect(act, &QAction::triggered, this, [this, path]() {
                    onMasterProductSelected(path); });
            } else {
                connect(act, &QAction::triggered, this, [this, path]() {
                    onSlaveProductSelected(path); });
            }
        }
    }
    return menu;
}

void ApplicationController::onMasterProductSelected(const QString& productPath)
{
    auto it = mProductRegistry.find(productPath);
    if (it == mProductRegistry.end()) return;
    mSelectedMasterPath = productPath;
    const ProductSourceInfo& info = it.value();

    RegistrationParams& rp = mMainWindow->regParams();
    rp.masterProductPath = info.productPath;
    rp.masterDisplayName = info.displayName;
    rp.masterPath = info.productPath;
    rp.masterOrbitVectors = info.orbitVectors;
    rp.masterDoppler = info.doppler;
    rp.masterRangeSpacing = info.sensorInfo.rangeSpacing;
    rp.masterAzimuthSpacing = info.sensorInfo.azimuthSpacing;
    rp.masterNearRange = info.sensorInfo.nearRange;
    rp.masterPrf = info.sensorInfo.prf;
    rp.wavelength = info.sensorInfo.wavelength;
    // 产品模式 (UI 策略页显示用; 运行时以服务内 ProductDetector 检测为准)
    int burstCount = info.bands.isEmpty() ? 0 : info.bands.first().burstCount;
    rp.productMode = ProductDetector::detect(info.sensorInfo, burstCount);

    mMainWindow->updateImageSelectionLabel(mMainWindow->masterInfoLabel(), info.displayName);
    qDebug() << "[Reg] Master product:" << info.displayName;

    // 配对数检查
    if (!mSelectedSlavePath.isEmpty()) {
        auto sit = mProductRegistry.find(mSelectedSlavePath);
        if (sit != mProductRegistry.end()) {
            mMainWindow->updateImageSelectionLabel(
                mMainWindow->slaveInfoLabel(),
                QStringLiteral("%1 (%2对)")
                    .arg(sit->displayName)
                    .arg(std::min(info.bands.size(), sit->bands.size())));
        }
    }

    ProcessingMonitorPanel* m = mMainWindow->processingMonitorPanel();
    if (m) m->appendLog(QStringLiteral("主产品: %1 Orbit%2 %3波段")
        .arg(info.displayName).arg(info.sensorInfo.relativeOrbit)
        .arg(info.bands.size()), "#4A90D9");
}

void ApplicationController::onSlaveProductSelected(const QString& productPath)
{
    auto it = mProductRegistry.find(productPath);
    if (it == mProductRegistry.end()) return;
    mSelectedSlavePath = productPath;
    const ProductSourceInfo& info = it.value();

    RegistrationParams& rp = mMainWindow->regParams();
    rp.slaveProductPath = info.productPath;
    rp.slaveDisplayName = info.displayName;
    rp.slavePath = info.productPath;
    rp.slaveOrbitVectors = info.orbitVectors;
    rp.slaveDoppler = info.doppler;
    rp.wavelength = info.sensorInfo.wavelength;

    // 配对信息
    int pairs = 0;
    if (!mSelectedMasterPath.isEmpty()) {
        auto mit = mProductRegistry.find(mSelectedMasterPath);
        if (mit != mProductRegistry.end())
            pairs = std::min(mit->bands.size(), info.bands.size());
    }

    QString infoText = info.displayName;
    if (pairs > 0) infoText += QStringLiteral(" (%1对)").arg(pairs);
    mMainWindow->updateImageSelectionLabel(mMainWindow->slaveInfoLabel(), infoText);
    qDebug() << "[Reg] Slave product:" << info.displayName;

    ProcessingMonitorPanel* m = mMainWindow->processingMonitorPanel();
    if (m) {
        m->appendLog(QStringLiteral("辅产品: %1 Orbit%2 %3波段")
            .arg(info.displayName).arg(info.sensorInfo.relativeOrbit)
            .arg(info.bands.size()), "#4A90D9");

        if (!mSelectedMasterPath.isEmpty()) {
            auto mit = mProductRegistry.find(mSelectedMasterPath);
            if (mit != mProductRegistry.end()) {
                if (info.sensorInfo.relativeOrbit != mit->sensorInfo.relativeOrbit)
                    m->appendLog(QStringLiteral("⚠ 轨道号不同 (%1 vs %2)!")
                        .arg(mit->sensorInfo.relativeOrbit)
                        .arg(info.sensorInfo.relativeOrbit), "#E67E22");
                else
                    m->appendLog(QStringLiteral("✓ 轨道号一致, 可配准 %1 对波段").arg(pairs), "#4CAF50");
            }
        }
    }
}

// ──────────────────────────────────────────────────────────
// 配准执行 / 基线估算
// ──────────────────────────────────────────────────────────
void ApplicationController::onRegistrationRunRequested(const RegistrationParams& params)
{
    mRegistrationSvc->setParams(params);
    // SentinelDataReader 已绕过 GDAL VSI, 与画布 GeoTIFF 转换无 VSI 冲突
    // 直接启动配准, 无需等待 mPendingLoadCount
    QtConcurrent::run([this]() {
        mRegistrationSvc->execute();
    });
}

void ApplicationController::onInterferogramRunRequested(const InterferogramParams& params)
{
    mInterferogramSvc->setParams(params);
    QtConcurrent::run([this]() { mInterferogramSvc->execute(); });
}

void ApplicationController::onBaselineEstimateRequested()
{
    RegistrationParams p = mMainWindow->collectRegParams();
    p.estimateBaseline = true;
    mRegistrationSvc->setParams(p);
    QtConcurrent::run([this]() {
        mRegistrationSvc->execute();
    });
}

// ──────────────────────────────────────────────────────────
// SAR 产品打开
// ──────────────────────────────────────────────────────────
void ApplicationController::onSarProductOpenRequested(const QString& path)
{
    if (mShuttingDown) return;

    qDebug() << "[Controller] onSarProductOpenRequested called with:" << path;

    ProcessingMonitorPanel* monitor = mMainWindow->processingMonitorPanel();
    if (monitor)
        monitor->appendLog(QStringLiteral("正在加载产品..."), "#4A90D9");
    QApplication::processEvents(); // 刷新 UI

    QScopedPointer<ISarProduct> product(createSarProduct(path));
    if (!product || !product->open(path)) {
        QMessageBox::warning(mMainWindow, QStringLiteral("打开失败"),
            QStringLiteral("无法识别该 Sentinel-1 产品。"));
        return;
    }
    qDebug() << "[Controller] product opened OK, bands:" << product->bands().size();

    SarSensorInfo sensorInfo = product->sensorInfo();
    QList<OrbitStateVector> orbitVectors = product->orbitStateVectors();
    DopplerInfo doppler = product->dopplerCentroid();

    // 构建 ProductSourceInfo
    ProductSourceInfo prodInfo;
    prodInfo.productPath = path;
    prodInfo.bands = product->bands();
    prodInfo.sensorInfo = sensorInfo;
    prodInfo.orbitVectors = orbitVectors;
    prodInfo.doppler = doppler;
    prodInfo.annotation = product->annotation();
    prodInfo.annotationsBySwath = product->allAnnotations();
    QString shortDate = sensorInfo.acquisitionStart.toString("MMdd");
    prodInfo.displayName = QStringLiteral("%1_%2 Orbit%3")
        .arg(sensorInfo.missionId.isEmpty() ? sensorInfo.sensorType : sensorInfo.missionId)
        .arg(shortDate)
        .arg(sensorInfo.relativeOrbit);
    mProductRegistry[path] = prodInfo;

    qDebug() << "========== [Registration Data] Product Loaded ==========";
    qDebug() << "  Display Name:" << prodInfo.displayName;
    qDebug() << "  wavelength     =" << sensorInfo.wavelength << "m";
    qDebug() << "  incidenceAngle =" << sensorInfo.incidenceAngleMid << "deg";
    qDebug() << "  nearRange      =" << sensorInfo.nearRange << "m";
    qDebug() << "  rangeSpacing   =" << sensorInfo.rangeSpacing << "m";
    qDebug() << "  azimuthSpacing =" << sensorInfo.azimuthSpacing << "m";
    qDebug() << "  prf            =" << sensorInfo.prf << "Hz";
    qDebug() << "==========================================================";

    const auto& bands = prodInfo.bands;

    // 图层
    LayerPanel* layerPanel = mMainWindow->layerPanel();
    if (layerPanel && !bands.isEmpty()) {
        mPendingGroupName = QStringLiteral("%1 %2 %3 Orbit%4")
            .arg(sensorInfo.missionId.isEmpty() ? sensorInfo.sensorType : sensorInfo.missionId)
            .arg(sensorInfo.acquisitionMode)
            .arg(sensorInfo.acquisitionStart.toString("yyyy-MM-dd"))
            .arg(sensorInfo.relativeOrbit);
        QgsProject::instance()->layerTreeRoot()->insertGroup(0, mPendingGroupName);

        QStringList paths;
        for (const auto& b : bands) {
            paths.append(b.rasterPath);
            mPendingProductRegistry[b.rasterPath] = path;
        }
        emit layerPanel->layerAddRequested(paths);
    }

    // 元数据面板
    SarMetadataPanel* metaPanel = mMainWindow->sarMetadataPanel();
    if (metaPanel) {
        metaPanel->setMetadata(sensorInfo.sensorType,
            sensorInfo.acquisitionStart.toString("yyyy-MM-dd hh:mm"),
            sarProductTypeToString(sensorInfo.productType),
            sensorInfo.polarizations.join(","), sensorInfo.wavelength,
            sensorInfo.rangeSpacing, sensorInfo.azimuthSpacing,
            sensorInfo.nearRange, sensorInfo.farRange, sensorInfo.prf,
            sensorInfo.centerFreq, sensorInfo.orbitDirection,
            sensorInfo.relativeOrbit, sensorInfo.acquisitionMode);
        QVector<QString> swNames;
        QVector<double> swNearR, swFarR, swRgSpac;
        for (const auto& b : bands) {
            if (b.nearRange <= 0) continue;
            if (!swNames.contains(b.subSwath)) {
                swNames.append(b.subSwath);
                swNearR.append(b.nearRange);
                double farR = b.nearRange + (b.samplesPerBurst - 1) * b.rangeSpacing;
                swFarR.append(farR);
                swRgSpac.append(b.rangeSpacing);
            }
        }
        if (swNames.size() > 1)
            metaPanel->setBandRangeInfo(swNames, swNearR, swFarR, swRgSpac);
    }

    // 监控面板
    if (monitor) {
        QStringList bandInfo;
        for (const auto& b : bands)
            bandInfo.append(QStringLiteral("  %1 %2 %3×%4")
                .arg(b.polarization).arg(b.subSwath)
                .arg(b.rasterSize.width()).arg(b.rasterSize.height()));
        monitor->appendLog(QStringLiteral("已加载: %1\n波段:\n%2")
            .arg(prodInfo.displayName).arg(bandInfo.join("\n")), "#4CAF50");
    }

    // 自动选择
    if (mPendingAutoSelect == 1) {
        onMasterProductSelected(path); mPendingAutoSelect = -1;
    } else if (mPendingAutoSelect == 0) {
        onSlaveProductSelected(path); mPendingAutoSelect = -1;
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
    canvas->setLayers(visible);
    canvas->refresh();

    // 自动选择已在 onSarProductOpenRequested 中处理，此处不再重复

    if (mMainWindow->masterButton() && !mProductRegistry.isEmpty())
        mMainWindow->masterButton()->setMenu(buildProductMenu(true));
    if (mMainWindow->slaveButton() && !mProductRegistry.isEmpty())
        mMainWindow->slaveButton()->setMenu(buildProductMenu(false));
}

void ApplicationController::shutdown()
{
    if (mShuttingDown) return;
    mShuttingDown = true;

    if (mMainWindow && mMainWindow->mapCanvasWidget()) {
        QgsMapCanvas* canvas = mMainWindow->mapCanvasWidget()->mapCanvas();
        if (canvas) {
            canvas->stopRendering(); canvas->waitWhileRendering();
            canvas->setLayers(QList<QgsMapLayer*>()); canvas->refresh();
        }
    }

    QgsProject* project = QgsProject::instance();
    if (project) {
        QMap<QString, QgsMapLayer*> layers = project->mapLayers();
        for (auto it = layers.constBegin(); it != layers.constEnd(); ++it)
            project->removeMapLayer(it.key());
    }

    mProductRegistry.clear();
    mPendingProductRegistry.clear();
    SentinelProductManager::instance().clear();
    mShuttingDown = false;
}
