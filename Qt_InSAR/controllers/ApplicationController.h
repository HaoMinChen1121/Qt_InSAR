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
#include "domain/params/InterferogramParams.h"
#include "domain/params/FilterUnwrapParams.h"
#include "domain/product/ProductManager.h"
#include "dataaccess/ISarProduct.h"
#include "dataaccess/annotation/SlcAnnotation.h"

class MainWindow;
class WorkerManager;
class QMenu;

class IRegistrationService;
class IInterferogramService;
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
    SlcAnnotation annotation;         // 完整 XML annotation (供元数据对话框等使用)
    QMap<QString, SlcAnnotation> annotationsBySwath; // 每波段独立数据, key="IW1/VH"
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
    IFilterService* filterService() const;
    IUnwrappingService* unwrappingService() const;
    IGeocodingService* geocodingService() const;

    // ── 产品注册与查询 ──
    QMap<QString, ProductSourceInfo> loadedProducts() const { return mProductRegistry; }
    QMenu* buildProductMenu(bool isMaster);

    // Product 驱动注册表 (qsar 产品 → 类型化包装)
    ProductManager* productManager() { return &mProductManager; }

private:
    void createServices();
    void wireConnections();
    void rebuildCanvasLayers();
    // 滤波→解缠链: 逐极化推进 (filter finished → unwrap → 下一极化)
    void runNextFilterUnwrap();

private slots:
    void onSarProductOpenRequested(const QString& path);
    void onRegistrationRunRequested(const RegistrationParams& params);
    void onBaselineEstimateRequested();
    void onInterferogramRunRequested(const InterferogramParams& params);
    void onFilterUnwrapRunRequested(const FilterUnwrapParams& params);
    void onMasterProductSelected(const QString& productPath);
    void onSlaveProductSelected(const QString& productPath);

private:
    MainWindow* mMainWindow;
    WorkerManager* mWorkerManager = nullptr;

    std::unique_ptr<IRegistrationService> mRegistrationSvc;
    std::unique_ptr<IInterferogramService> mInterferogramSvc;
    std::unique_ptr<IFilterService> mFilterSvc;
    std::unique_ptr<IUnwrappingService> mUnwrappingSvc;
    std::unique_ptr<IGeocodingService> mGeocodingSvc;

    // 产品注册表: productPath → ProductSourceInfo
    QMap<QString, ProductSourceInfo> mProductRegistry;

    // qsar 产品注册表 (Product 驱动, schema v2.0)
    ProductManager mProductManager;

    // 待关联: 波段路径 → 产品路径 (等待 QGIS 分配 layer ID)
    QMap<QString, QString> mPendingProductRegistry;

    // 主/辅产品选择
    QString mSelectedMasterPath;
    QString mSelectedSlavePath;

    // 当前正在加载的产品分组名
    QString mPendingGroupName;

    // 待自动选择: -1=无, 0=辅, 1=主
    int mPendingAutoSelect = -1;

    // 滤波完成后的链式解缠参数 (filter → unwrap 流程)
    UnwrappingParams mPendingUnwrapParams;
    // 滤波→解缠链的逐极化队列 (一次运行处理产品全部极化)
    QStringList mPendingFilterPols;
    FilterParams mPendingFilterBase;

    // 待完成的异步加载计数
    int mPendingLoadCount = 0;

    bool mShuttingDown = false;
};

#endif // APPLICATIONCONTROLLER_H
