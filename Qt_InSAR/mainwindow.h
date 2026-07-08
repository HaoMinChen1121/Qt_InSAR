#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "SARibbonMainWindow.h"

class SARibbonCategory;
class SARibbonQuickAccessBar;
class SARibbonButtonGroupWidget;
class SARibbonPanel;
class QCloseEvent;
class QLineEdit;

class MapCanvasWidget;
class LayerPanel;
class ProcessingMonitorPanel;
class OverviewWidget;
class SarMetadataPanel;
class QDockWidget;

class ApplicationController;

class MainWindow : public SARibbonMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

    LayerPanel* layerPanel() const { return mLayerPanel; }
    MapCanvasWidget* mapCanvasWidget() const { return mMapCanvasWidget; }
    ProcessingMonitorPanel* processingMonitorPanel() const { return mMonitorPanel; }
    OverviewWidget* overviewWidget() const { return mOverviewWidget; }
    SarMetadataPanel* sarMetadataPanel() const { return mSarMetadataPanel; }

    void setAppController(ApplicationController* ctrl) { mAppController = ctrl; }
    ApplicationController* appController() const { return mAppController; }

private:
    // 文件页
    void createCategoryFile(SARibbonCategory* page);
    // 影像配准页
    void createCategoryRegistration(SARibbonCategory* page);
    // 干涉图生成页
    void createCategoryInterferogram(SARibbonCategory* page);
    // 滤波与解缠页
    void createCategoryFilterUnwrap(SARibbonCategory* page);
    // 地理编码页
    void createCategoryGeocoding(SARibbonCategory* page);
    // 形变分析页
    void createCategoryDeformation(SARibbonCategory* page);
    // 工作流页
    void createCategoryWorkflow(SARibbonCategory* page);
    // 批处理页
    void createCategoryBatch(SARibbonCategory* page);

    void createQuickAccessBar();
    void createRightButtonGroup();
    QAction* createAction(const QString& text, const QString& iconurl, const QString& objName);
    QAction* createAction(const QString& text, const QString& iconurl);

    void initUI();
    void initMapCanvas();
    void initLayerPanel();
    void initMonitorPanel();
    void initOverviewPanel();
    void initSarMetadataPanel();

    MapCanvasWidget* mMapCanvasWidget = nullptr;
    QDockWidget* mLayerDock = nullptr;
    LayerPanel* mLayerPanel = nullptr;
    QDockWidget* mMonitorDock = nullptr;
    ProcessingMonitorPanel* mMonitorPanel = nullptr;
    QDockWidget* mOverviewDock = nullptr;
    OverviewWidget* mOverviewWidget = nullptr;
    QDockWidget* mSarMetadataDock = nullptr;
    SarMetadataPanel* mSarMetadataPanel = nullptr;

signals:
    /// Emitted when the user selects a SAR product file to open.
    void sarProductOpenRequested(const QString& path);

private slots:
    void onActionHelpTriggered();
    void onUndoActionTriggered();
    void onRedoActionTriggered();
    void onSearchEditorEditingFinished();

protected:
    void closeEvent(QCloseEvent* closeEvent) override;

private:
    QLineEdit* mSearchEditor = nullptr;
    ApplicationController* mAppController = nullptr;
};

#endif // MAINWINDOW_H
