#include "mainwindow.h"
#include "controllers/ApplicationController.h"
#include "controllers/WorkerManager.h"
#include "algorithms/SimdMath.h"
#include "services/registration/steps/DataReader.h"
#include <qgsapplication.h>
#include <gdal_priv.h>
#include <QDebug>

int main(int argc, char *argv[])
{
    GDALAllRegister();
    CPLSetConfigOption("GDAL_CACHEMAX", "512");
    CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");

    sar::gSimdLevel = sar::detectSimdLevel();
    qDebug() << "[SIMD] detected level:" << static_cast<int>(sar::gSimdLevel)
             << "(0=Scalar 1=SSE2 2=AVX2   max=" << static_cast<int>(sar::kMaxSimd) << ")";
    QgsApplication app(argc, argv, true);
    QgsApplication::setApplicationName("InSAR Processor");
    QgsApplication::setPrefixPath("E:/GIS_QT/apps/qgis-ltr", true);
    QgsApplication::initQgis();

    // 清空 profiling 文件 (在 QApp 创建之后)
    QString exeDir = QCoreApplication::applicationDirPath();
    for (const auto& fn : {"profile_coarse.txt", "profile_fine.txt", "profile_sinc.txt"})
        QFile(exeDir + "/" + fn).remove();

    MainWindow mainWindow;
    ApplicationController controller(&mainWindow);
    mainWindow.setAppController(&controller);
    controller.initialize();
    mainWindow.show();

    int ret = app.exec();
    QgsApplication::exitQgis();
    GDALDestroyDriverManager();
    DataReader::cleanupExtracted();
    return ret;
}
