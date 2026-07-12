#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QMap>
#include <memory>

#include "domain/SlcImage.h"
#include "domain/SarSensorInfo.h"
#include "domain/OrbitInfo.h"
#include "domain/params/RegistrationParams.h"

class MainWindow;
class WorkerManager;
class QMenu;

class IRegistrationService;
class IInterferogramService;
class IFlatEarthService;
class IDifferentialService;
class IFilterService;
class IUnwrappingService;
class IGeocodingService;

// 已加载 SLC 波段的来源信息
struct SlcSourceInfo {
    QString   productPath;            // SAFE/zip 根路径
    QString   bandPath;               // 原始复数 TIFF 路径
    QString   displayName;            // 显示名 (如 "IW1_VV")
    int       bandIndex = 0;
    SlcImage  slcImage;               // 基础元数据
    SarSensorInfo sensorInfo;         // 传感器全量元数据
    QList<OrbitStateVector> orbitVectors;
    DopplerInfo doppler;
};

class ApplicationController : public QObject
{
    Q_OBJECT
public:
    explicit ApplicationController(MainWindow* mainWindow, QObject* parent = nullptr);
    ~ApplicationController();

    void initialize();
    void shutdown();

    // Service accessors
    IRegistrationService* registrationService() const;
    IInterferogramService* interferogramService() const;
    IFlatEarthService* flatEarthService() const;
    IDifferentialService* differentialService() const;
    IFilterService* filterService() const;
    IUnwrappingService* unwrappingService() const;
    IGeocodingService* geocodingService() const;

    // ── SLC 图层注册与查询 ──
    QMap<QString, SlcSourceInfo> loadedSlcImages() const { return mSlcRegistry; }
    QMenu* buildSlcLayerMenu(bool isMaster);

private:
    void createServices();
    void wireConnections();
    void rebuildCanvasLayers();

private slots:
    void onSarProductOpenRequested(const QString& path);
    void onRegistrationRunRequested(const RegistrationParams& params);
    void onBaselineEstimateRequested(const QString& masterPath,
                                     const QString& slavePath);
    void onMasterImageSelected(const QString& layerId);
    void onSlaveImageSelected(const QString& layerId);

private:
    MainWindow* mMainWindow;
    WorkerManager* mWorkerManager = nullptr;

    std::unique_ptr<IRegistrationService> mRegistrationSvc;
    std::unique_ptr<IInterferogramService> mInterferogramSvc;
    std::unique_ptr<IFlatEarthService> mFlatEarthSvc;
    std::unique_ptr<IDifferentialService> mDifferentialSvc;
    std::unique_ptr<IFilterService> mFilterSvc;
    std::unique_ptr<IUnwrappingService> mUnwrappingSvc;
    std::unique_ptr<IGeocodingService> mGeocodingSvc;

    // SLC 图层注册表: layerId → 来源信息
    QMap<QString, SlcSourceInfo> mSlcRegistry;

    // 待关联: 波段路径 → 来源信息 (等待 QGIS 分配 layer ID)
    QMap<QString, SlcSourceInfo> mPendingSlcRegistry;

    // 当前正在加载的产品分组名
    QString mPendingGroupName;

    // 主/辅影像选择
    QString mSelectedMasterLayerId;
    QString mSelectedSlaveLayerId;

    // 临时文件追踪
    QStringList mTempFiles;
    bool mShuttingDown = false;
};

#endif // APPLICATIONCONTROLLER_H
