#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "SARibbonMainWindow.h"
#include "domain/params/RegistrationParams.h"
#include "domain/params/InterferogramParams.h"

class SARibbonCategory;
class SARibbonQuickAccessBar;
class SARibbonButtonGroupWidget;
class SARibbonPanel;
class QCloseEvent;
class QLineEdit;
class QToolButton;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QMenu;

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

    // ── 配准控件访问器 (供 ApplicationController 使用) ──
    RegistrationParams& regParams() { return mRegParams; }
    QToolButton* masterButton() const { return mBtnMaster; }
    QToolButton* slaveButton() const { return mBtnSlave; }
    QLabel* masterInfoLabel() const { return mLblMasterInfo; }
    QLabel* slaveInfoLabel() const { return mLblSlaveInfo; }
    void updateImageSelectionLabel(QLabel* label, const QString& path);

    // ── 配准辅助方法 ──
    RegistrationParams collectRegParams() const;
    void applyParamsToRibbon(const RegistrationParams& p);

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

    // ── Ribbon 控件 (影像配准页) ──
    QToolButton*     mBtnMaster = nullptr;
    QToolButton*     mBtnSlave = nullptr;
    QLabel*          mLblMasterInfo = nullptr;
    QLabel*          mLblSlaveInfo = nullptr;
    QComboBox*       mCoarseMethodCombo = nullptr;
    QComboBox*       mFineMethodCombo = nullptr;
    QSpinBox*        mGcpSpin = nullptr;
    QSpinBox*        mSearchWinSpin = nullptr;
    QDoubleSpinBox*  mCorrThreshSpin = nullptr;
    QComboBox*       mResampleCombo = nullptr;
    QLabel*          mOutputDirLabel = nullptr;
    QCheckBox*       mKeepResCheck = nullptr;
    RegistrationParams mRegParams;
    InterferogramParams mIfgParams;

    QMenu* buildSlcLayerMenu(bool isMaster);

signals:
    /// 用户触发配准执行 (携带打包好的参数)
    void registrationRunRequested(const RegistrationParams& params);
    /// 用户触发基线快速估算
    void baselineEstimateRequested();
    void interferogramRunRequested(const InterferogramParams& params);
};

#endif // MAINWINDOW_H
