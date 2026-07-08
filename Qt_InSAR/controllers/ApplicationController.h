#ifndef APPLICATIONCONTROLLER_H
#define APPLICATIONCONTROLLER_H

#include <QObject>
#include <QStringList>
#include <memory>

class MainWindow;
class WorkerManager;

class IRegistrationService;
class IInterferogramService;
class IFlatEarthService;
class IDifferentialService;
class IFilterService;
class IUnwrappingService;
class IGeocodingService;

class ISlcReader;
class ISlcWriter;
class IInterferogramReader;
class IInterferogramWriter;
class IDemReader;
class IOrbitDataReader;

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

private:
    void createServices();
    void createDataAccess();
    void wireConnections();
    void rebuildCanvasLayers();

private slots:
    void onSarProductOpenRequested(const QString& path);

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

    // 临时文件追踪 (/vsizip/ 提取的本地 GTiff)
    QStringList mTempFiles;
    bool mShuttingDown = false;
};

#endif // APPLICATIONCONTROLLER_H
