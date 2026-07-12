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
#include "dataaccess/ISarProduct.h"

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

// 已加载产品的来源信息
struct ProductSourceInfo {
    QString   productPath;            // SAFE/zip 根路径
    QString   displayName;            // 显示名 (如 "S1A_0605 Orbit87")
    QList<SarBandDescriptor> bands;   // 所有波段
    SarSensorInfo sensorInfo;         // 传感器元数据
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

    // ── 产品注册与查询 ──
    QMap<QString, ProductSourceInfo> loadedProducts() const { return mProductRegistry; }
    QMenu* buildProductMenu(bool isMaster);

private:
    void createServices();
    void wireConnections();
    void rebuildCanvasLayers();

private slots:
    void onSarProductOpenRequested(const QString& path);
    void onRegistrationRunRequested(const RegistrationParams& params);
    void onBaselineEstimateRequested();
    void onMasterProductSelected(const QString& productPath);
    void onSlaveProductSelected(const QString& productPath);

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

    // 产品注册表: productPath → ProductSourceInfo
    QMap<QString, ProductSourceInfo> mProductRegistry;

    // 待关联: 波段路径 → 产品路径 (等待 QGIS 分配 layer ID)
    QMap<QString, QString> mPendingProductRegistry;

    // 主/辅产品选择
    QString mSelectedMasterPath;
    QString mSelectedSlavePath;

    // 当前正在加载的产品分组名
    QString mPendingGroupName;

    // 待自动选择: -1=无, 0=辅, 1=主
    int mPendingAutoSelect = -1;

    // 临时文件追踪
    QStringList mTempFiles;
    bool mShuttingDown = false;
};

#endif // APPLICATIONCONTROLLER_H
