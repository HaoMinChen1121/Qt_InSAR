#include "mainwindow.h"
#include "SARibbonBar.h"
#include "SARibbonButtonGroupWidget.h"
#include "SARibbonCategory.h"
#include "SARibbonMenu.h"
#include "SARibbonPanel.h"
#include "SARibbonQuickAccessBar.h"

#include "ui/MapCanvasWidget.h"
#include "ui/LayerPanel.h"
#include "ui/ProcessingMonitorPanel.h"
#include "ui/OverviewWidget.h"
#include "ui/SarMetadataPanel.h"
#include "ui/Registration/RegistrationDialog.h"
#include "ui/Interferogram/InterferogramDialog.h"
#include "ui/Filter/FilterUnwrappingDialog.h"
#include "ui/Geocoding/GeocodingDialog.h"
#include "ui/Deformation/DeformationDialog.h"

#include "controllers/ApplicationController.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QDebug>
#include <QMessageBox>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QCheckBox>
#include <QFont>
#include <QFormLayout>
#include <QPushButton>
#include <QGridLayout>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : SARibbonMainWindow(parent)
{
    initUI();
}

MainWindow::~MainWindow() = default;

void MainWindow::initUI()
{
    setWindowTitle(QStringLiteral("InSAR数据处理系统[*]"));
    setWindowModified(true);
    setWindowIcon(QIcon(":/icon/icon/InSar.svg"));

    initMapCanvas();
    initLayerPanel();
    initMonitorPanel();
    initOverviewPanel();
    initSarMetadataPanel();

    SARibbonBar* ribbonBar = this->ribbonBar();
    setContentsMargins(1, 0, 1, 0);
    ribbonBar->setContentsMargins(4, 0, 4, 0);

    SARibbonCategory* catFile = ribbonBar->addCategoryPage(QStringLiteral("文件"));
    catFile->setObjectName("catFile");
    createCategoryFile(catFile);

    SARibbonCategory* catReg = ribbonBar->addCategoryPage(QStringLiteral("影像配准"));
    catReg->setObjectName("catRegistration");
    createCategoryRegistration(catReg);

    SARibbonCategory* catIfg = ribbonBar->addCategoryPage(QStringLiteral("干涉图生成"));
    catIfg->setObjectName("catInterferogram");
    createCategoryInterferogram(catIfg);

    SARibbonCategory* catFilt = ribbonBar->addCategoryPage(QStringLiteral("滤波与解缠"));
    catFilt->setObjectName("catFilterUnwrap");
    createCategoryFilterUnwrap(catFilt);

    SARibbonCategory* catGeo = ribbonBar->addCategoryPage(QStringLiteral("地理编码"));
    catGeo->setObjectName("catGeocoding");
    createCategoryGeocoding(catGeo);

    SARibbonCategory* catDef = ribbonBar->addCategoryPage(QStringLiteral("形变分析"));
    catDef->setObjectName("catDeformation");
    createCategoryDeformation(catDef);

    SARibbonCategory* catWF = ribbonBar->addCategoryPage(QStringLiteral("工作流"));
    catWF->setObjectName("catWorkflow");
    createCategoryWorkflow(catWF);

    SARibbonCategory* catBatch = ribbonBar->addCategoryPage(QStringLiteral("批处理"));
    catBatch->setObjectName("catBatch");
    createCategoryBatch(catBatch);

    createQuickAccessBar();
    createRightButtonGroup();

    setMinimumWidth(1100);
    setRibbonTheme(SARibbonTheme::RibbonThemeOffice2021Blue);
    ribbonBar->setRibbonStyle(SARibbonBar::RibbonStyleLooseThreeRow);
    QFont ribbonFont = ribbonBar->font();
    ribbonFont.setPointSize(10);
    ribbonBar->setFont(ribbonFont);
    resize(1400, 900);
    showMaximized();
}

void MainWindow::initMapCanvas()
{
    mMapCanvasWidget = new MapCanvasWidget(this);
    setCentralWidget(mMapCanvasWidget);

    connect(mMapCanvasWidget, &MapCanvasWidget::canvasExtentChanged,
            this, [this](const QgsRectangle& /*extent*/) {
    });
    connect(mMapCanvasWidget, &MapCanvasWidget::mapClicked,
            this, [this](const QgsPointXY& /*point*/) {
        mMonitorPanel->appendLog(QStringLiteral("地图点击"), "#888888");
    });
}

void MainWindow::initLayerPanel()
{
    mLayerDock = new QDockWidget(QStringLiteral("图层"), this);
    mLayerDock->setObjectName("LayerDock");
    mLayerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mLayerDock->setMinimumWidth(200);
    mLayerPanel = new LayerPanel(mLayerDock);
    mLayerDock->setWidget(mLayerPanel);
    addDockWidget(Qt::LeftDockWidgetArea, mLayerDock);
}

void MainWindow::initMonitorPanel()
{
    mMonitorDock = new QDockWidget(QStringLiteral("处理监控"), this);
    mMonitorDock->setObjectName("MonitorDock");
    mMonitorDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    mMonitorDock->setMinimumHeight(100);
    mMonitorPanel = new ProcessingMonitorPanel(mMonitorDock);
    mMonitorDock->setWidget(mMonitorPanel);
    addDockWidget(Qt::BottomDockWidgetArea, mMonitorDock);
}

void MainWindow::initOverviewPanel()
{
    mOverviewDock = new QDockWidget(QStringLiteral("鹰眼视图"), this);
    mOverviewDock->setObjectName("OverviewDock");
    mOverviewDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mOverviewDock->setMinimumSize(200, 150);

    mOverviewWidget = new OverviewWidget(mOverviewDock);
    mOverviewWidget->setMainCanvas(mMapCanvasWidget->mapCanvas());
    mOverviewDock->setWidget(mOverviewWidget);

    addDockWidget(Qt::LeftDockWidgetArea, mOverviewDock);

    connect(mMapCanvasWidget, &MapCanvasWidget::canvasExtentChanged,
            mOverviewWidget, &OverviewWidget::onMainExtentChanged);

    connect(mOverviewWidget, &OverviewWidget::extentChangeRequested,
            mMapCanvasWidget, [this](const QgsRectangle& extent) {
        mMapCanvasWidget->setCanvasExtent(extent);
    });
}

void MainWindow::initSarMetadataPanel()
{
    mSarMetadataDock = new QDockWidget(QStringLiteral("SAR元数据"), this);
    mSarMetadataDock->setObjectName("SarMetadataDock");
    mSarMetadataDock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    mSarMetadataDock->setMinimumWidth(280);
    mSarMetadataDock->setMaximumWidth(600);

    mSarMetadataPanel = new SarMetadataPanel(mSarMetadataDock);
    mSarMetadataDock->setWidget(mSarMetadataPanel);

    addDockWidget(Qt::RightDockWidgetArea, mSarMetadataDock);
    resizeDocks({mSarMetadataDock}, {300}, Qt::Horizontal);
}

QAction* MainWindow::createAction(const QString& text, const QString& iconurl, const QString& objName)
{
    QAction* act = new QAction(QIcon(iconurl), text, this);
    act->setObjectName(objName);
    return act;
}

QAction* MainWindow::createAction(const QString& text, const QString& iconurl)
{
    return new QAction(QIcon(iconurl), text, this);
}

// ========================================================================
// 文件 — 按传感器类型打开SAR产品
// ========================================================================
void MainWindow::createCategoryFile(SARibbonCategory* page)
{
    SARibbonPanel* pnlSensor = page->addPanel(QStringLiteral("打开SAR传感器影像"));

    QAction* actOpenS1 = createAction(QStringLiteral("Sentinel-1\n(SLC/GRD)"),
                                       ":/icon/icon/folder-star.svg", "actOpenS1");
    pnlSensor->addLargeAction(actOpenS1);
    connect(actOpenS1, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this,
            QStringLiteral("选择 Sentinel-1 产品"),
            QString(),
            QStringLiteral("Sentinel-1 产品 (*.zip *.SAFE);;"
                           "所有文件 (*.*)"));
        if (path.isEmpty()) return;

        emit sarProductOpenRequested(path);
    });

    QAction* actOpenNisar = createAction(QStringLiteral("NISAR\n(L/S-band)"),
                                          ":/icon/icon/folder-checkmark.svg", "actOpenNisar");
    pnlSensor->addLargeAction(actOpenNisar);
    connect(actOpenNisar, &QAction::triggered, this, [this]() {
        QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择 NISAR 产品"),
            QString(), QStringLiteral("NISAR 产品 (*.h5 *.nc);;所有文件 (*.*)"));
        if (!file.isEmpty() && mLayerPanel)
            emit mLayerPanel->layerAddRequested({file});
    });

    QAction* actOpenAlos = createAction(QStringLiteral("ALOS-2/4\n(PALSAR)"),
                                         ":/icon/icon/folder-table.svg", "actOpenAlos");
    pnlSensor->addLargeAction(actOpenAlos);
    connect(actOpenAlos, &QAction::triggered, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 ALOS-2 CEOS 产品目录"));
        if (!dir.isEmpty() && mLayerPanel)
            emit mLayerPanel->layerAddRequested({dir});
    });

    QAction* actOpenTsx = createAction(QStringLiteral("TerraSAR-X\n(TDX/PAZ)"),
                                        ":/icon/icon/file.svg", "actOpenTsx");
    pnlSensor->addLargeAction(actOpenTsx);
    connect(actOpenTsx, &QAction::triggered, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择 TerraSAR-X 产品目录"));
        if (!dir.isEmpty() && mLayerPanel)
            emit mLayerPanel->layerAddRequested({dir});
    });

    QAction* actOpenGf3 = createAction(QStringLiteral("高分三号\n(C-band)"),
                                        ":/icon/icon/Align-Center.svg", "actOpenGf3");
    pnlSensor->addLargeAction(actOpenGf3);
    connect(actOpenGf3, &QAction::triggered, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择高分三号产品目录"));
        if (!dir.isEmpty() && mLayerPanel)
            emit mLayerPanel->layerAddRequested({dir});
    });

    SARibbonPanel* pnlGeneral = page->addPanel(QStringLiteral("通用"));

    QAction* actAddRaster = createAction(QStringLiteral("添加通用\n栅格图层"),
                                          ":/icon/icon/save.svg", "actAddRaster");
    pnlGeneral->addLargeAction(actAddRaster);
    connect(actAddRaster, &QAction::triggered, this, [this]() {
        if (mLayerPanel)
            mLayerPanel->onAddLayer();
    });

    QAction* actOpen = createAction(QStringLiteral("打开\n项目"), ":/icon/icon/folder-stats.svg", "actOpen");
    pnlGeneral->addLargeAction(actOpen);
    QAction* actSave = createAction(QStringLiteral("保存\n项目"), ":/icon/icon/save.svg", "actSave");
    pnlGeneral->addLargeAction(actSave);

    QAction* actExit = createAction(QStringLiteral("退出"), ":/icon/icon/delete.svg", "actExit");
    pnlGeneral->addLargeAction(actExit);
    connect(actExit, &QAction::triggered, this, []() { qApp->quit(); });
}

void MainWindow::createQuickAccessBar()
{
    SARibbonBar* bar = ribbonBar();
    if (!bar) return;
    SARibbonQuickAccessBar* qab = bar->quickAccessBar();
    if (!qab) return;
    qab->addAction(createAction(QStringLiteral("保存"), ":/icon/icon/save.svg", "actSave"));
    qab->addAction(createAction(QStringLiteral("撤销"), ":/icon/icon/undo.svg", "actUndo"));
    qab->addAction(createAction(QStringLiteral("重做"), ":/icon/icon/redo.svg", "actRedo"));
}

void MainWindow::createRightButtonGroup()
{
    SARibbonBar* bar = ribbonBar();
    if (!bar) return;
    SARibbonButtonGroupWidget* group = bar->rightButtonGroup();
    if (!group) return;
    QAction* actHelp = createAction(QStringLiteral("帮助"), ":/icon/icon/help.svg", "actHelp");
    group->addAction(actHelp);
    connect(actHelp, &QAction::triggered, this, &MainWindow::onActionHelpTriggered);
}

// ========================================================================
// 影像配准
// ========================================================================
void MainWindow::createCategoryRegistration(SARibbonCategory* page)
{
    // ── Panel 1: 影像选择 ──
    SARibbonPanel* pnlIO = page->addPanel(QStringLiteral("影像选择"));

    QAction* actMaster = createAction(QStringLiteral("主产品\n(点击选择)"),
                                       ":/icon/icon/save.svg", "actMaster");
    pnlIO->addLargeAction(actMaster);
    connect(actMaster, &QAction::triggered, this, [this]() {
        if (!mAppController) return;
        QMenu* menu = mAppController->buildProductMenu(true);
        menu->exec(mapToGlobal(QPoint(width() / 4, height() / 3)));
        menu->deleteLater();
    });

    mLblMasterInfo = new QLabel(QStringLiteral("主: 未选择"), this);
    mLblMasterInfo->setWordWrap(true);
    mLblMasterInfo->setMaximumHeight(36);
    QFont infoFont = mLblMasterInfo->font();
    infoFont.setPointSize(8);
    mLblMasterInfo->setFont(infoFont);
    pnlIO->addSmallWidget(mLblMasterInfo);

    QAction* actSlave = createAction(QStringLiteral("辅产品\n(点击选择)"),
                                      ":/icon/icon/Align-Left.svg", "actSlave");
    pnlIO->addLargeAction(actSlave);
    connect(actSlave, &QAction::triggered, this, [this]() {
        if (!mAppController) return;
        QMenu* menu = mAppController->buildProductMenu(false);
        menu->exec(mapToGlobal(QPoint(width() / 4, height() / 3)));
        menu->deleteLater();
    });

    mLblSlaveInfo = new QLabel(QStringLiteral("辅: 未选择"), this);
    mLblSlaveInfo->setWordWrap(true);
    mLblSlaveInfo->setMaximumHeight(36);
    QFont slaveInfoFont = mLblSlaveInfo->font();
    slaveInfoFont.setPointSize(8);
    mLblSlaveInfo->setFont(slaveInfoFont);
    pnlIO->addSmallWidget(mLblSlaveInfo);

    // ── Panel 2: 配准策略 ──
    SARibbonPanel* pnlMethod = page->addPanel(QStringLiteral("配准策略"));

    mCoarseMethodCombo = new QComboBox(this);
    mCoarseMethodCombo->addItem(QStringLiteral("轨道法"), "Orbit");
    mCoarseMethodCombo->addItem(QStringLiteral("互相关"), "CrossCorrelation");
    mCoarseMethodCombo->setMinimumWidth(140);
    QFont comboFont = mCoarseMethodCombo->font();
    comboFont.setPointSize(8);
    mCoarseMethodCombo->setFont(comboFont);
    pnlMethod->addSmallWidget(mCoarseMethodCombo);

    // ── Panel 3: 执行 ──
    SARibbonPanel* pnlExec = page->addPanel(QStringLiteral("执行"));

    QAction* actExecReg = createAction(QStringLiteral("运行配准"),
                                        ":/icon/icon/folder-cog.svg", "actExecReg");
    pnlExec->addLargeAction(actExecReg);
    connect(actExecReg, &QAction::triggered, this, [this]() {
        if (mRegParams.masterProductPath.isEmpty()
            || mRegParams.slaveProductPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("请先选择主影像和辅影像"));
            return;
        }
        RegistrationParams p = collectRegParams();
        mMonitorPanel->appendLog(
            QStringLiteral("影像配准已启动...\n  主: %1\n  辅: %2")
                .arg(mRegParams.masterDisplayName,
                     mRegParams.slaveDisplayName),
            "#4A90D9");
        emit registrationRunRequested(p);
    });

    QAction* actAdvReg = createAction(QStringLiteral("高级参数"),
                                       ":/icon/icon/layout.svg", "actAdvReg");
    pnlExec->addSmallAction(actAdvReg);
    connect(actAdvReg, &QAction::triggered, this, [this]() {
        RegistrationDialog dlg(this);
        dlg.setParams(collectRegParams());
        if (dlg.exec() == QDialog::Accepted) {
            mRegParams = dlg.params();
            applyParamsToRibbon(mRegParams);
        }
    });

    QAction* actBaseline = createAction(QStringLiteral("基线估算"),
                                         ":/icon/icon/redo.svg", "actBaseline");
    pnlExec->addSmallAction(actBaseline);
    connect(actBaseline, &QAction::triggered, this, [this]() {
        if (mRegParams.masterProductPath.isEmpty()
            || mRegParams.slaveProductPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("请先选择主产品和辅产品"));
            return;
        }
        emit baselineEstimateRequested();
    });

    // ── 初始参数 ──
    mRegParams.coarseMethod = "Orbit";
    mRegParams.fineMethod = "SubPixel";
    mRegParams.resamplingMethod = "Sinc";
    mRegParams.outputPrefix = "registered";
}

// ── 配准参数收集 ──
RegistrationParams MainWindow::collectRegParams() const
{
    RegistrationParams p = mRegParams;
    if (mCoarseMethodCombo)
        p.coarseMethod = mCoarseMethodCombo->currentData().toString();
    return p;
}

// ── 从对话框回写参数到 Ribbon ──
void MainWindow::applyParamsToRibbon(const RegistrationParams& p)
{
    if (mCoarseMethodCombo) {
        int idx = mCoarseMethodCombo->findData(p.coarseMethod);
        if (idx >= 0) mCoarseMethodCombo->setCurrentIndex(idx);
    }
    // 保留产品路径和元数据
    QString mp = mRegParams.masterProductPath;
    QString sp = mRegParams.slaveProductPath;
    QString md = mRegParams.masterDisplayName;
    QString sd = mRegParams.slaveDisplayName;
    mRegParams = p;
    mRegParams.masterProductPath = mp;
    mRegParams.slaveProductPath = sp;
    mRegParams.masterDisplayName = md;
    mRegParams.slaveDisplayName = sd;
    mRegParams.masterPath = mp;
    mRegParams.slavePath = sp;
}

// ── 更新主/辅影像选择标签 ──
void MainWindow::updateImageSelectionLabel(QLabel* label, const QString& path)
{
    QString text = label == mLblMasterInfo ? QStringLiteral("主: ") : QStringLiteral("辅: ");
    QFileInfo fi(path);
    QString name = fi.fileName();
    if (name.isEmpty()) name = path.section('/', -1);
    text += name;
    label->setText(text);
    label->setToolTip(path);
    label->repaint();
}

// ========================================================================
// 干涉图生成
// ========================================================================
void MainWindow::createCategoryInterferogram(SARibbonCategory* page)
{
    SARibbonPanel* pnlIfg = page->addPanel(QStringLiteral("干涉参数"));
    QSpinBox* rgSpin = new QSpinBox(this); rgSpin->setRange(1, 32); rgSpin->setValue(1);
    rgSpin->setPrefix(QStringLiteral("Rg: "));
    pnlIfg->addSmallWidget(rgSpin);
    QSpinBox* azSpin = new QSpinBox(this); azSpin->setRange(1, 32); azSpin->setValue(1);
    azSpin->setPrefix(QStringLiteral("Az: "));
    pnlIfg->addSmallWidget(azSpin);
    QComboBox* outTypeCombo = new QComboBox(this);
    outTypeCombo->addItems({QStringLiteral("复数"), QStringLiteral("相位"), QStringLiteral("相干性")});
    pnlIfg->addSmallWidget(outTypeCombo);

    SARibbonPanel* pnlFlat = page->addPanel(QStringLiteral("平地效应"));
    QComboBox* refCombo = new QComboBox(this);
    refCombo->addItems({QStringLiteral("椭球面"), QStringLiteral("轨道"), QStringLiteral("外部DEM")});
    pnlFlat->addSmallWidget(refCombo);
    QAction* actOrbit = createAction(QStringLiteral("轨道文件"), ":/icon/icon/save.svg", "actOrbit");
    pnlFlat->addLargeAction(actOrbit);

    SARibbonPanel* pnlDiff = page->addPanel(QStringLiteral("差分干涉"));
    QAction* actDem = createAction(QStringLiteral("DEM文件"), ":/icon/icon/save.svg", "actDemDiff");
    pnlDiff->addLargeAction(actDem);
    QComboBox* dirCombo = new QComboBox(this);
    dirCombo->addItems({QStringLiteral("LOS"), QStringLiteral("垂直向")});
    pnlDiff->addSmallWidget(dirCombo);

    SARibbonPanel* pnlExec = page->addPanel(QStringLiteral("执行"));
    QAction* actExecIfg = createAction(QStringLiteral("生成干涉图"),
                                        ":/icon/icon/folder-cog.svg", "actExecIfg");
    pnlExec->addLargeAction(actExecIfg);
    connect(actExecIfg, &QAction::triggered, this, [this]() {
        mMonitorPanel->appendLog(QStringLiteral("干涉图生成已启动..."), "#4A90D9");
    });
    QAction* actAdvIfg = createAction(QStringLiteral("高级参数"), ":/icon/icon/layout.svg", "actAdvIfg");
    pnlExec->addSmallAction(actAdvIfg);
    connect(actAdvIfg, &QAction::triggered, this, [this]() {
        InterferogramDialog dlg(this); dlg.exec();
    });
}

// ========================================================================
// 滤波与解缠
// ========================================================================
void MainWindow::createCategoryFilterUnwrap(SARibbonCategory* page)
{
    SARibbonPanel* pnlFilt = page->addPanel(QStringLiteral("滤波"));
    QComboBox* filtMethodCombo = new QComboBox(this);
    filtMethodCombo->addItems({QStringLiteral("Goldstein"), QStringLiteral("Baran")});
    pnlFilt->addSmallWidget(filtMethodCombo);
    QDoubleSpinBox* alphaSpin = new QDoubleSpinBox(this);
    alphaSpin->setRange(0.1, 1.0); alphaSpin->setSingleStep(0.1); alphaSpin->setValue(0.5);
    alphaSpin->setPrefix(QStringLiteral("α: "));
    pnlFilt->addSmallWidget(alphaSpin);
    QSpinBox* winSpin = new QSpinBox(this);
    winSpin->setRange(8, 256); winSpin->setValue(32);
    winSpin->setPrefix(QStringLiteral("窗口: "));
    pnlFilt->addSmallWidget(winSpin);

    SARibbonPanel* pnlUnwrap = page->addPanel(QStringLiteral("相位解缠"));
    QComboBox* unwrapMethodCombo = new QComboBox(this);
    unwrapMethodCombo->addItems({QStringLiteral("枝切法"), QStringLiteral("最小二乘")});
    pnlUnwrap->addSmallWidget(unwrapMethodCombo);
    QDoubleSpinBox* cohSpin = new QDoubleSpinBox(this);
    cohSpin->setRange(0, 1); cohSpin->setSingleStep(0.05); cohSpin->setValue(0.3);
    cohSpin->setPrefix(QStringLiteral("相干: "));
    pnlUnwrap->addSmallWidget(cohSpin);

    QDoubleSpinBox* wlSpin = new QDoubleSpinBox(this);
    wlSpin->setDecimals(4); wlSpin->setRange(0.001, 1.0); wlSpin->setValue(0.03125);
    wlSpin->setPrefix(QStringLiteral("λ: "));
    pnlUnwrap->addSmallWidget(wlSpin);
    QDoubleSpinBox* incSpin = new QDoubleSpinBox(this);
    incSpin->setRange(0, 90); incSpin->setValue(35.0);
    incSpin->setPrefix(QStringLiteral("入射角: "));
    pnlUnwrap->addSmallWidget(incSpin);

    SARibbonPanel* pnlExec = page->addPanel(QStringLiteral("执行"));
    QAction* actExecFW = createAction(QStringLiteral("运行滤波解缠"),
                                       ":/icon/icon/folder-cog.svg", "actExecFW");
    pnlExec->addLargeAction(actExecFW);
    connect(actExecFW, &QAction::triggered, this, [this]() {
        mMonitorPanel->appendLog(QStringLiteral("滤波与解缠已启动..."), "#4A90D9");
    });
    QAction* actAdvFW = createAction(QStringLiteral("高级参数"), ":/icon/icon/layout.svg", "actAdvFW");
    pnlExec->addSmallAction(actAdvFW);
    connect(actAdvFW, &QAction::triggered, this, [this]() {
        FilterUnwrappingDialog dlg(this); dlg.exec();
    });
}

// ========================================================================
// 地理编码
// ========================================================================
void MainWindow::createCategoryGeocoding(SARibbonCategory* page)
{
    SARibbonPanel* pnlMethod = page->addPanel(QStringLiteral("地理编码方法"));
    QComboBox* geoMethodCombo = new QComboBox(this);
    geoMethodCombo->addItems({QStringLiteral("距离多普勒"), QStringLiteral("多项式")});
    pnlMethod->addSmallWidget(geoMethodCombo);
    QAction* actDemGeo = createAction(QStringLiteral("DEM\n(地形)"), ":/icon/icon/save.svg", "actDemGeo");
    pnlMethod->addLargeAction(actDemGeo);

    SARibbonPanel* pnlCrs = page->addPanel(QStringLiteral("坐标系统"));
    QSpinBox* epsgSpin = new QSpinBox(this);
    epsgSpin->setRange(1024, 32767); epsgSpin->setValue(4326);
    epsgSpin->setPrefix(QStringLiteral("EPSG: "));
    pnlCrs->addSmallWidget(epsgSpin);
    QDoubleSpinBox* resSpin = new QDoubleSpinBox(this);
    resSpin->setDecimals(4); resSpin->setRange(0, 1000); resSpin->setValue(0);
    resSpin->setPrefix(QStringLiteral("分辨率: "));
    pnlCrs->addSmallWidget(resSpin);
    QComboBox* fmtCombo = new QComboBox(this);
    fmtCombo->addItems({QStringLiteral("GeoTIFF"), QStringLiteral("ENVI")});
    pnlCrs->addSmallWidget(fmtCombo);

    SARibbonPanel* pnlExec = page->addPanel(QStringLiteral("执行"));
    QAction* actExecGeo = createAction(QStringLiteral("运行地理编码"),
                                        ":/icon/icon/folder-cog.svg", "actExecGeo");
    pnlExec->addLargeAction(actExecGeo);
    connect(actExecGeo, &QAction::triggered, this, [this]() {
        mMonitorPanel->appendLog(QStringLiteral("地理编码已启动..."), "#4A90D9");
    });
    QAction* actAdvGeo = createAction(QStringLiteral("高级参数"), ":/icon/icon/layout.svg", "actAdvGeo");
    pnlExec->addSmallAction(actAdvGeo);
    connect(actAdvGeo, &QAction::triggered, this, [this]() {
        GeocodingDialog dlg(this); dlg.exec();
    });
}

// ========================================================================
// 形变分析 (新增)
// ========================================================================
void MainWindow::createCategoryDeformation(SARibbonCategory* page)
{
    SARibbonPanel* pnlDef = page->addPanel(QStringLiteral("形变计算"));
    QComboBox* convCombo = new QComboBox(this);
    convCombo->addItems({QStringLiteral("相位→形变"), QStringLiteral("相位→高程")});
    pnlDef->addSmallWidget(convCombo);
    QComboBox* projCombo = new QComboBox(this);
    projCombo->addItems({QStringLiteral("LOS方向"), QStringLiteral("垂直向"), QStringLiteral("水平向")});
    pnlDef->addSmallWidget(projCombo);

    SARibbonPanel* pnlTS = page->addPanel(QStringLiteral("时序分析"));
    QComboBox* tsMethodCombo = new QComboBox(this);
    tsMethodCombo->addItems({QStringLiteral("Stacking"), QStringLiteral("SBAS"), QStringLiteral("PS-InSAR")});
    pnlTS->addSmallWidget(tsMethodCombo);
    QComboBox* atmCombo = new QComboBox(this);
    atmCombo->addItems({QStringLiteral("GACOS"), QStringLiteral("线性模型"), QStringLiteral("无")});
    pnlTS->addSmallWidget(atmCombo);

    SARibbonPanel* pnlExec = page->addPanel(QStringLiteral("执行"));
    QAction* actExecDef = createAction(QStringLiteral("运行形变分析"),
                                        ":/icon/icon/folder-cog.svg", "actExecDef");
    pnlExec->addLargeAction(actExecDef);
    connect(actExecDef, &QAction::triggered, this, [this]() {
        mMonitorPanel->appendLog(QStringLiteral("形变分析已启动..."), "#4A90D9");
    });
    QAction* actAdvDef = createAction(QStringLiteral("高级参数"), ":/icon/icon/layout.svg", "actAdvDef");
    pnlExec->addSmallAction(actAdvDef);
    connect(actAdvDef, &QAction::triggered, this, [this]() {
        DeformationDialog dlg(this); dlg.exec();
    });
}

// ========================================================================
// 工作流
// ========================================================================
void MainWindow::createCategoryWorkflow(SARibbonCategory* page)
{
    SARibbonPanel* pnl = page->addPanel(QStringLiteral("工程管理"));
    QAction* actNew = createAction(QStringLiteral("新建工程"), ":/icon/icon/file.svg", "actNewWF");
    pnl->addLargeAction(actNew);
    QAction* actSave = createAction(QStringLiteral("保存工程"), ":/icon/icon/save.svg", "actSaveWF");
    pnl->addLargeAction(actSave);
    QAction* actLoad = createAction(QStringLiteral("打开工程"), ":/icon/icon/folder-stats.svg", "actLoadWF");
    pnl->addLargeAction(actLoad);
    QAction* actRun = createAction(QStringLiteral("运行工作流"), ":/icon/icon/folder-cog.svg", "actRunWF");
    pnl->addLargeAction(actRun);
}

// ========================================================================
// 批处理
// ========================================================================
void MainWindow::createCategoryBatch(SARibbonCategory* page)
{
    SARibbonPanel* pnlTask = page->addPanel(QStringLiteral("像对管理"));
    QAction* actAdd = createAction(QStringLiteral("添加像对"), ":/icon/icon/save.svg", "actAddPair");
    pnlTask->addLargeAction(actAdd);
    QAction* actImport = createAction(QStringLiteral("导入列表"), ":/icon/icon/folder-stats.svg", "actImportList");
    pnlTask->addLargeAction(actImport);
    QAction* actRemove = createAction(QStringLiteral("移除"), ":/icon/icon/delete.svg", "actRemoveBatch");
    pnlTask->addSmallAction(actRemove);
    QAction* actClear = createAction(QStringLiteral("清空队列"), ":/icon/icon/disable.svg", "actClearBatch");
    pnlTask->addSmallAction(actClear);

    SARibbonPanel* pnlControl = page->addPanel(QStringLiteral("执行控制"));
    QAction* actStartBatch = createAction(QStringLiteral("开始全部"), ":/icon/icon/folder-cog.svg", "actStartBatch");
    pnlControl->addLargeAction(actStartBatch);
    QAction* actPause = createAction(QStringLiteral("暂停"), ":/icon/icon/disable.svg", "actPause");
    pnlControl->addSmallAction(actPause);
    QAction* actResume = createAction(QStringLiteral("继续"), ":/icon/icon/undo.svg", "actResume");
    pnlControl->addSmallAction(actResume);
    QAction* actCancelBatch = createAction(QStringLiteral("取消"), ":/icon/icon/delete.svg", "actCancelBatch");
    pnlControl->addSmallAction(actCancelBatch);
    QAction* actRetry = createAction(QStringLiteral("重试"), ":/icon/icon/redo.svg", "actRetry");
    pnlControl->addSmallAction(actRetry);

    SARibbonPanel* pnlReport = page->addPanel(QStringLiteral("报告"));
    QAction* actReport = createAction(QStringLiteral("导出报告"), ":/icon/icon/file.svg", "actReport");
    pnlReport->addLargeAction(actReport);
    QAction* actDetail = createAction(QStringLiteral("详细面板"), ":/icon/icon/layout.svg", "actDetail");
    pnlReport->addLargeAction(actDetail);
}

// ========================================================================
// Slots
// ========================================================================
void MainWindow::onActionHelpTriggered()
{
    QMessageBox::about(this, QStringLiteral("关于"),
        QStringLiteral("InSAR数据处理系统 v0.1\nQt + QGIS + SARibbon\n\n"
                       "功能模块：影像配准 | 干涉图生成 | 滤波与解缠 | 地理编码 | 形变分析\n"
                       "可选模块：工作流设计器 | 批处理引擎"));
}

void MainWindow::onUndoActionTriggered()
{
    mMonitorPanel->appendLog(QStringLiteral("撤销"), "#888888");
}

void MainWindow::onRedoActionTriggered()
{
    mMonitorPanel->appendLog(QStringLiteral("重做"), "#888888");
}

void MainWindow::onSearchEditorEditingFinished()
{
    QString text = mSearchEditor ? mSearchEditor->text() : QString();
    if (!text.isEmpty())
        mMonitorPanel->appendLog(QStringLiteral("搜索: %1").arg(text), "#888888");
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    qDebug() << "[InSAR] 关闭窗口, 开始清理...";

    // 1. 清空处理监控
    if (mMonitorPanel) {
        mMonitorPanel->appendLog(QStringLiteral("程序正在关闭，清理资源..."), "#E67E22");
    }

    // 2. 通知控制器清理 (清画布 → 删图层 → 删VRT)
    if (mAppController) {
        mAppController->shutdown();
    }

    event->accept();
    qDebug() << "[InSAR] 窗口关闭完成";
}
