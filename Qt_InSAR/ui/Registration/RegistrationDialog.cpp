#include "RegistrationDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>

RegistrationDialog::RegistrationDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("影像配准参数"));
    setMinimumSize(640, 520);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget(this);

    // ===== Tab 1: 主辅影像 =====
    QWidget* tab1 = new QWidget;
    QFormLayout* form1 = new QFormLayout(tab1);
    mMasterPath = new QLineEdit;
    mSlavePath = new QLineEdit;
    QPushButton* masterBrowse = new QPushButton(tr("浏览..."));
    QPushButton* slaveBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* masterLayout = new QHBoxLayout;
    masterLayout->addWidget(mMasterPath, 1);
    masterLayout->addWidget(masterBrowse);
    QHBoxLayout* slaveLayout = new QHBoxLayout;
    slaveLayout->addWidget(mSlavePath, 1);
    slaveLayout->addWidget(slaveBrowse);
    form1->addRow(tr("主影像:"), masterLayout);
    form1->addRow(tr("辅影像:"), slaveLayout);
    mMasterMeta = new QLabel(tr("未加载"));
    mSlaveMeta = new QLabel(tr("未加载"));
    form1->addRow(tr("主影像信息:"), mMasterMeta);
    form1->addRow(tr("辅影像信息:"), mSlaveMeta);
    tabs->addTab(tab1, tr("主辅影像"));

    // ===== Tab 2: 配准策略 =====
    QWidget* tab2 = new QWidget;
    QVBoxLayout* layout2 = new QVBoxLayout(tab2);

    // ── 路线选择 ──
    QGroupBox* routeGroup = new QGroupBox(tr("配准路线"));
    QHBoxLayout* routeLayout = new QHBoxLayout(routeGroup);

    mRouteGroup = new QButtonGroup(this);
    mRouteGroup->setExclusive(true);

    mRoute1Btn = new QRadioButton(tr("◇ 快速 — 轨道+FFT幅度\n   浏览产品 / 非高精度应用"));
    mRoute2Btn = new QRadioButton(tr("△ 稳健 — NCC+FFTW相位相关\n   高稳定性 / 复杂区域"));
    mRoute3Btn = new QRadioButton(tr("★ 标准 — FFT幅度+相位相关 (推荐)\n   Sentinel-1 IW TOPS SLC 标准处理"));

    mRouteGroup->addButton(mRoute1Btn, 0);
    mRouteGroup->addButton(mRoute2Btn, 1);
    mRouteGroup->addButton(mRoute3Btn, 2);
    mRoute3Btn->setChecked(true);

    routeLayout->addWidget(mRoute1Btn);
    routeLayout->addWidget(mRoute2Btn);
    routeLayout->addWidget(mRoute3Btn);
    layout2->addWidget(routeGroup);

    // ── 堆叠参数页 ──
    mRouteStack = new QStackedWidget;
    mRouteStack->addWidget(createRoute1Page());
    mRouteStack->addWidget(createRoute2Page());
    mRouteStack->addWidget(createRoute3Page());
    mRouteStack->setCurrentIndex(2); // 默认标准路线
    layout2->addWidget(mRouteStack);

    // ── ESD (路线2/3共享) ──
    mEnableEsd = new QCheckBox(tr("ESD增强频谱分集 (TOPSAR方位向精化至<0.001像素)"));
    mEnableEsd->setChecked(true);
    layout2->addWidget(mEnableEsd);

    layout2->addStretch();
    tabs->addTab(tab2, tr("配准策略"));

    // ===== Tab 3: 重采样 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* form3 = new QFormLayout(tab3);
    mResamplingMethod = new QComboBox;
    mResamplingMethod->addItem("Sinc", "Sinc");
    mResamplingMethod->addItem(tr("双线性"), "Bilinear");
    mResamplingMethod->addItem(tr("双三次"), "Bicubic");
    form3->addRow(tr("重采样方法:"), mResamplingMethod);
    mSincWindow = new QSpinBox;
    mSincWindow->setRange(4, 64);
    mSincWindow->setValue(16);
    form3->addRow(tr("Sinc 窗半径:"), mSincWindow);
    mSincBeta = new QDoubleSpinBox;
    mSincBeta->setRange(1.0, 10.0);
    mSincBeta->setSingleStep(0.5);
    mSincBeta->setValue(2.5);
    form3->addRow(tr("Kaiser \xce\xb2:"), mSincBeta);
    mOutResRange = new QDoubleSpinBox;
    mOutResRange->setDecimals(4);
    mOutResRange->setRange(0, 9999);
    mOutResRange->setValue(0);
    mOutResRange->setSpecialValueText(tr("保持原始"));
    form3->addRow(tr("距离向分辨率:"), mOutResRange);
    mOutResAzimuth = new QDoubleSpinBox;
    mOutResAzimuth->setDecimals(4);
    mOutResAzimuth->setRange(0, 9999);
    mOutResAzimuth->setValue(0);
    mOutResAzimuth->setSpecialValueText(tr("保持原始"));
    form3->addRow(tr("方位向分辨率:"), mOutResAzimuth);
    tabs->addTab(tab3, tr("重采样"));

    // ===== Tab 4: 输出 =====
    QWidget* tab4 = new QWidget;
    QFormLayout* form4 = new QFormLayout(tab4);
    mOutputDir = new QLineEdit;
    QPushButton* dirBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* dirLayout = new QHBoxLayout;
    dirLayout->addWidget(mOutputDir, 1);
    dirLayout->addWidget(dirBrowse);
    form4->addRow(tr("输出目录:"), dirLayout);
    mOutputPrefix = new QLineEdit("registered");
    form4->addRow(tr("文件前缀:"), mOutputPrefix);
    mEstimateBaseline = new QCheckBox(tr("配准前估算基线"));
    mEstimateBaseline->setChecked(true);
    form4->addRow(mEstimateBaseline);
    tabs->addTab(tab4, tr("输出"));

    mainLayout->addWidget(tabs);

    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);

    // 路线切换联动
    connect(mRouteGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &RegistrationDialog::onRouteChanged);

    connect(masterBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择主影像"));
        if (!f.isEmpty()) mMasterPath->setText(f);
    });
    connect(slaveBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择辅影像"));
        if (!f.isEmpty()) mSlavePath->setText(f);
    });
    connect(dirBrowse, &QPushButton::clicked, this, [this]() {
        QString d = QFileDialog::getExistingDirectory(this, tr("选择输出目录"));
        if (!d.isEmpty()) mOutputDir->setText(d);
    });
}

// ── 三个路线的参数页面 ──

QWidget* RegistrationDialog::createRoute1Page()
{
    QWidget* page = new QWidget;
    QFormLayout* f = new QFormLayout(page);
    f->addRow(new QLabel(tr("粗配准: 轨道几何预测 + 小窗口FFT幅度域互相关")));

    mR1CoarseFFT = new QSpinBox;
    mR1CoarseFFT->setRange(32, 256);
    mR1CoarseFFT->setValue(64);
    mR1CoarseFFT->setPrefix(tr("粗窗口: "));
    f->addRow(tr("FFT窗口大小:"), mR1CoarseFFT);

    mR1ControlPoints = new QSpinBox;
    mR1ControlPoints->setRange(16, 128);
    mR1ControlPoints->setValue(32);
    f->addRow(tr("控制点数(每burst):"), mR1ControlPoints);

    QLabel* note = new QLabel(tr("注: 无精配准步骤，适合快速浏览。TOPSAR模式下精度有限。"));
    note->setWordWrap(true);
    note->setStyleSheet("color: #888;");
    f->addRow(note);
    return page;
}

QWidget* RegistrationDialog::createRoute2Page()
{
    QWidget* page = new QWidget;
    QFormLayout* f = new QFormLayout(page);
    f->addRow(new QLabel(tr("粗配准: 幅度NCC滑动窗口搜索")));

    mR2NccWindow = new QSpinBox;
    mR2NccWindow->setRange(32, 256);
    mR2NccWindow->setValue(128);
    mR2NccWindow->setPrefix(tr("NCC窗口: "));
    f->addRow(tr("NCC窗口大小:"), mR2NccWindow);

    mR2SearchWindow = new QSpinBox;
    mR2SearchWindow->setRange(8, 512);
    mR2SearchWindow->setValue(64);
    mR2SearchWindow->setPrefix(tr("搜索半径: \xc2\xb1"));
    f->addRow(tr("NCC搜索范围:"), mR2SearchWindow);

    mR2ControlPoints = new QSpinBox;
    mR2ControlPoints->setRange(16, 512);
    mR2ControlPoints->setValue(64);
    f->addRow(tr("控制点数(每burst):"), mR2ControlPoints);

    f->addRow(new QLabel(tr("精配准: FFTW3复数域相位相关")));

    mR2FineWindow = new QSpinBox;
    mR2FineWindow->setRange(64, 512);
    mR2FineWindow->setValue(256);
    mR2FineWindow->setPrefix(tr("精窗口: "));
    f->addRow(tr("精配准窗口:"), mR2FineWindow);

    mR2CorrThreshold = new QDoubleSpinBox;
    mR2CorrThreshold->setRange(0.0, 1.0);
    mR2CorrThreshold->setSingleStep(0.05);
    mR2CorrThreshold->setValue(0.3);
    f->addRow(tr("相关性阈值:"), mR2CorrThreshold);

    mR2PolyDegree = new QComboBox;
    mR2PolyDegree->addItem("1", 1);
    mR2PolyDegree->addItem("2", 2);
    mR2PolyDegree->addItem("3", 3);
    mR2PolyDegree->setCurrentIndex(1);
    f->addRow(tr("多项式阶数:"), mR2PolyDegree);

    mR2EnableFineFFT = new QCheckBox(tr("启用FFTW3精配准"));
    mR2EnableFineFFT->setChecked(true);
    f->addRow(mR2EnableFineFFT);

    mR2FineFFTWindow = new QSpinBox;
    mR2FineFFTWindow->setRange(64, 512);
    mR2FineFFTWindow->setValue(256);
    mR2FineFFTWindow->setPrefix(tr("窗口: "));
    f->addRow(tr("FFTW3窗口:"), mR2FineFFTWindow);

    return page;
}

QWidget* RegistrationDialog::createRoute3Page()
{
    QWidget* page = new QWidget;
    QFormLayout* f = new QFormLayout(page);
    f->addRow(new QLabel(tr("粗配准: 幅度域大窗口FFT互相关 + 亚像素峰值")));

    mR3CoarseFFT = new QSpinBox;
    mR3CoarseFFT->setRange(64, 512);
    mR3CoarseFFT->setValue(256);
    mR3CoarseFFT->setPrefix(tr("粗窗口: "));
    f->addRow(tr("FFT窗口大小:"), mR3CoarseFFT);

    mR3ControlPoints = new QSpinBox;
    mR3ControlPoints->setRange(16, 512);
    mR3ControlPoints->setValue(64);
    f->addRow(tr("控制点数(每burst):"), mR3ControlPoints);

    f->addRow(new QLabel(tr("精配准: FFTW3复数域相位相关 + RANSAC多项式拟合")));

    mR3FineWindow = new QSpinBox;
    mR3FineWindow->setRange(64, 512);
    mR3FineWindow->setValue(256);
    mR3FineWindow->setPrefix(tr("精窗口: "));
    f->addRow(tr("精配准窗口:"), mR3FineWindow);

    mR3CorrThreshold = new QDoubleSpinBox;
    mR3CorrThreshold->setRange(0.0, 1.0);
    mR3CorrThreshold->setSingleStep(0.05);
    mR3CorrThreshold->setValue(0.3);
    f->addRow(tr("相关性阈值:"), mR3CorrThreshold);

    mR3PolyDegree = new QComboBox;
    mR3PolyDegree->addItem("1", 1);
    mR3PolyDegree->addItem("2", 2);
    mR3PolyDegree->addItem("3", 3);
    mR3PolyDegree->setCurrentIndex(1);
    f->addRow(tr("多项式阶数:"), mR3PolyDegree);

    mR3EnableFineFFT = new QCheckBox(tr("启用FFTW3精配准"));
    mR3EnableFineFFT->setChecked(true);
    f->addRow(mR3EnableFineFFT);

    mR3FineFFTWindow = new QSpinBox;
    mR3FineFFTWindow->setRange(64, 512);
    mR3FineFFTWindow->setValue(256);
    mR3FineFFTWindow->setPrefix(tr("窗口: "));
    f->addRow(tr("FFTW3窗口:"), mR3FineFFTWindow);

    QLabel* note = new QLabel(tr("推荐参数: 窗口256×256, 点数64/burst, 多项式2阶。接近SNAP/ISCE标准流程。"));
    note->setWordWrap(true);
    note->setStyleSheet("color: #888;");
    f->addRow(note);
    return page;
}

void RegistrationDialog::onRouteChanged(int routeIdx)
{
    mRouteStack->setCurrentIndex(routeIdx);
    // 路线1无ESD
    mEnableEsd->setEnabled(routeIdx != 0);
    if (routeIdx == 0) mEnableEsd->setChecked(false);
}

// ── setParams / params ──

void RegistrationDialog::setParams(const RegistrationParams& p)
{
    mMetaHolder = p;

    mMasterPath->setText(p.masterPath);
    mSlavePath->setText(p.slavePath);

    auto metaText = [&p](bool isMaster) -> QString {
        const QString& displayName = isMaster ? p.masterDisplayName : p.slaveDisplayName;
        const QString& prodPath = isMaster ? p.masterProductPath : p.slaveProductPath;
        if (prodPath.isEmpty()) return QStringLiteral("未加载");
        const auto& orbits = isMaster ? p.masterOrbitVectors : p.slaveOrbitVectors;
        QString extra = orbits.isEmpty()
            ? QStringLiteral(" (无轨道数据)")
            : QStringLiteral(" (%1轨道点)").arg(orbits.size());
        return (displayName.isEmpty() ? QStringLiteral("已选择") : displayName) + extra;
    };
    mMasterMeta->setText(QStringLiteral("主: %1").arg(metaText(true)));
    mSlaveMeta->setText(QStringLiteral("辅: %1").arg(metaText(false)));

    // 路线
    int routeIdx = static_cast<int>(p.route);
    if (routeIdx < 0 || routeIdx > 2) routeIdx = 2;
    mRouteGroup->button(routeIdx)->setChecked(true);
    mRouteStack->setCurrentIndex(routeIdx);
    mEnableEsd->setEnabled(routeIdx != 0);

    // Route1
    mR1CoarseFFT->setValue(p.coarseWindowSize);
    mR1ControlPoints->setValue(p.offsetPerBurst);

    // Route2
    mR2NccWindow->setValue(p.coarseWindowSize);
    mR2SearchWindow->setValue(p.coarseSearchWindow);
    mR2ControlPoints->setValue(p.offsetPerBurst);
    mR2FineWindow->setValue(p.fineFFTWindow);
    mR2CorrThreshold->setValue(p.correlationThreshold);
    int idx = mR2PolyDegree->findData(p.polynomialDegree);
    if (idx >= 0) mR2PolyDegree->setCurrentIndex(idx);
    mR2EnableFineFFT->setChecked(p.enableFineFFT);
    mR2FineFFTWindow->setValue(p.fineFFTWindow);

    // Route3
    mR3CoarseFFT->setValue(p.coarseWindowSize);
    mR3ControlPoints->setValue(p.offsetPerBurst);
    mR3FineWindow->setValue(p.fineFFTWindow);
    mR3CorrThreshold->setValue(p.correlationThreshold);
    idx = mR3PolyDegree->findData(p.polynomialDegree);
    if (idx >= 0) mR3PolyDegree->setCurrentIndex(idx);
    mR3EnableFineFFT->setChecked(p.enableFineFFT);
    mR3FineFFTWindow->setValue(p.fineFFTWindow);

    // 重采样
    idx = mResamplingMethod->findData(p.resamplingMethod);
    if (idx >= 0) mResamplingMethod->setCurrentIndex(idx);
    mSincWindow->setValue(p.sincWindowSize);
    mSincBeta->setValue(p.sincBeta);
    mOutResRange->setValue(p.outputResolutionRange);
    mOutResAzimuth->setValue(p.outputResolutionAzimuth);

    // 输出
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
    mEstimateBaseline->setChecked(p.estimateBaseline);
    mEnableEsd->setChecked(p.enableEsd);
}

RegistrationParams RegistrationDialog::params() const
{
    RegistrationParams p = mMetaHolder;

    int routeIdx = mRouteGroup->checkedId();
    if (routeIdx < 0) routeIdx = 2;
    p.route = static_cast<RegRoute>(routeIdx);

    p.masterPath = mMasterPath->text();
    p.slavePath = mSlavePath->text();

    // 根据路线读取对应参数
    switch (p.route) {
    case RegRoute::Route1_OrbitFFT:
        p.coarseWindowSize = mR1CoarseFFT->value();
        p.offsetPerBurst = mR1ControlPoints->value();
        p.coarseMethod = "Orbit";
        p.fineMethod = "SubPixel";
        p.enableEsd = false;
        p.enableFineFFT = false;
        break;
    case RegRoute::Route2_NCC_FFTW:
        p.coarseWindowSize = mR2NccWindow->value();
        p.coarseSearchWindow = mR2SearchWindow->value();
        p.offsetPerBurst = mR2ControlPoints->value();
        p.fineFFTWindow = mR2FineWindow->value();
        p.correlationThreshold = mR2CorrThreshold->value();
        p.polynomialDegree = mR2PolyDegree->currentData().toInt();
        p.enableFineFFT = mR2EnableFineFFT->isChecked();
        p.fineFFTWindow = mR2FineFFTWindow->value();
        p.coarseMethod = "CrossCorrelation";
        p.fineMethod = "FFT";
        p.enableEsd = mEnableEsd->isChecked();
        break;
    case RegRoute::Route3_FFT_FFTW:
        p.coarseWindowSize = mR3CoarseFFT->value();
        p.offsetPerBurst = mR3ControlPoints->value();
        p.fineFFTWindow = mR3FineWindow->value();
        p.correlationThreshold = mR3CorrThreshold->value();
        p.polynomialDegree = mR3PolyDegree->currentData().toInt();
        p.enableFineFFT = mR3EnableFineFFT->isChecked();
        p.fineFFTWindow = mR3FineFFTWindow->value();
        p.coarseMethod = "FFT";
        p.fineMethod = "FFT";
        p.enableEsd = mEnableEsd->isChecked();
        break;
    }

    // 重采样
    p.resamplingMethod = mResamplingMethod->currentData().toString();
    p.sincWindowSize = mSincWindow->value();
    p.sincBeta = mSincBeta->value();
    p.outputResolutionRange = mOutResRange->value();
    p.outputResolutionAzimuth = mOutResAzimuth->value();

    // 输出
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    p.estimateBaseline = mEstimateBaseline->isChecked();

    return p;
}
