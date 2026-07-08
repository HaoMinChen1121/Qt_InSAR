#include "mainwindow.h"
#include "controllers/ApplicationController.h"
#include "controllers/WorkerManager.h"
#include <qgsapplication.h>
#include <gdal_priv.h>

int main(int argc, char *argv[])
{
    GDALAllRegister();
    QgsApplication app(argc, argv, true);
    QgsApplication::setApplicationName("InSAR Processor");
    QgsApplication::setPrefixPath("E:/GIS_QT/apps/qgis-ltr", true);
    QgsApplication::initQgis();

    MainWindow mainWindow;
    ApplicationController controller(&mainWindow);
    mainWindow.setAppController(&controller);
    controller.initialize();
    mainWindow.show();

    int ret = app.exec();
    QgsApplication::exitQgis();
    return ret;
}
