#include "RegistrationDialog.h"
#include "services/registration/strategy/StrategyFactory.h"
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
    setMinimumSize(640, 560);

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

    // ── 产品模式 (自动识别, 只读) ──
    mProductModeLabel = new QLabel(tr("未检测"));
    mProductModeLabel->setStyleSheet("font-weight: bold;");
    layout2->addWidget(mProductModeLabel);

    // ── 处理等级 ──
    QGroupBox* levelGroup = new QGroupBox(tr("处理等级"));
    QHBoxLayout* levelLayout = new QHBoxLayout(levelGroup);
    mLevelGroup = new QButtonGroup(this);
    mLevelGroup->setExclusive(true);
    mFastBtn = new QRadioButton(tr("◇ 快速\n   跳过精配准/ESD"));
    mStandardBtn = new QRadioButton(tr("★ 标准 (推荐)\n   完整流程"));
    mHighBtn = new QRadioButton(tr("◆ 高精度\n   更多控制点/高阶模型"));
    mLevelGroup->addButton(mFastBtn, static_cast<int>(ProcessingLevel::Fast));
    mLevelGroup->addButton(mStandardBtn, static_cast<int>(ProcessingLevel::Standard));
    mLevelGroup->addButton(mHighBtn, static_cast<int>(ProcessingLevel::High));
    mStandardBtn->setChecked(true);
    levelLayout->addWidget(mFastBtn);
    levelLayout->addWidget(mStandardBtn);
    levelLayout->addWidget(mHighBtn);
    layout2->addWidget(levelGroup);

    // ── 策略解释 (只读, 随等级/引擎联动) ──
    mStrategySummary = new QLabel;
    mStrategySummary->setWordWrap(true);
    mStrategySummary->setStyleSheet(
        "background: #f4f4f4; border: 1px solid #ddd; padding: 6px;");
    layout2->addWidget(mStrategySummary);

    // ── 粗配准引擎覆盖 ──
    QFormLayout* form2 = new QFormLayout;
    mCoarseEngineCombo = new QComboBox;
    mCoarseEngineCombo->addItem(tr("策略默认 (推荐)"), -1);
    mCoarseEngineCombo->addItem(QStringLiteral("FFT 幅度"), static_cast<int>(CorrelationMethod::FFT_AMPLITUDE));
    mCoarseEngineCombo->addItem(QStringLiteral("NCC"), static_cast<int>(CorrelationMethod::NCC));
    form2->addRow(tr("粗配准引擎:"), mCoarseEngineCombo);

    // ── 参数 (当前等级) ──
    mCoarseWindow = new QSpinBox;
    mCoarseWindow->setRange(32, 512);
    form2->addRow(tr("粗窗口:"), mCoarseWindow);

    mSearchWindow = new QSpinBox;
    mSearchWindow->setRange(8, 512);
    mSearchWindow->setPrefix(QStringLiteral("±"));
    form2->addRow(tr("NCC搜索半径:"), mSearchWindow);

    mFineWindow = new QSpinBox;
    mFineWindow->setRange(64, 512);
    form2->addRow(tr("精配准窗口:"), mFineWindow);

    mOffsetPerBurst = new QSpinBox;
    mOffsetPerBurst->setRange(4, 128);
    form2->addRow(tr("控制点数(每burst):"), mOffsetPerBurst);

    mCorrThreshold = new QDoubleSpinBox;
    mCorrThreshold->setRange(0.0, 1.0);
    mCorrThreshold->setSingleStep(0.05);
    form2->addRow(tr("相关性阈值:"), mCorrThreshold);

    mPolyDegree = new QComboBox;
    mPolyDegree->addItem(QStringLiteral("1 (常数)"), 1);
    mPolyDegree->addItem(QStringLiteral("2 (方位线性)"), 2);
    mPolyDegree->addItem(QStringLiteral("3 (含距离耦合)"), 3);
    form2->addRow(tr("方位多项式阶数:"), mPolyDegree);

    layout2->addLayout(form2);
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
    mSincWindow->setValue(8);
    form3->addRow(tr("Sinc 窗半径:"), mSincWindow);
    mSincBeta = new QDoubleSpinBox;
    mSincBeta->setRange(1.0, 10.0);
    mSincBeta->setSingleStep(0.5);
    mSincBeta->setValue(2.5);
    form3->addRow(tr("Kaiser \xce\xb2:"), mSincBeta);
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

    // 等级/引擎切换联动
    connect(mLevelGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &RegistrationDialog::onLevelChanged);
    connect(mCoarseEngineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateStrategyView(); });

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

// ── 等级切换: 保存当前等级参数 → 加载新等级 (每等级独立, 不互相覆盖) ──
void RegistrationDialog::onLevelChanged(int levelIdx)
{
    saveCurrentProfile();
    mActiveLevel = static_cast<ProcessingLevel>(levelIdx);
    loadProfile(mActiveLevel);
    updateStrategyView();
}

void RegistrationDialog::saveCurrentProfile() const
{
    RegistrationProfileParams& prof =
        mMetaHolder.profiles[static_cast<int>(mActiveLevel)];
    prof.coarseWindowSize = mCoarseWindow->value();
    prof.coarseSearchWindow = mSearchWindow->value();
    prof.fineWindowSize = mFineWindow->value();
    prof.offsetPerBurst = mOffsetPerBurst->value();
    prof.correlationThreshold = mCorrThreshold->value();
    prof.polynomialDegree = mPolyDegree->currentData().toInt();
}

void RegistrationDialog::loadProfile(ProcessingLevel level)
{
    const RegistrationProfileParams& prof =
        mMetaHolder.profiles[static_cast<int>(level)];
    mCoarseWindow->setValue(prof.coarseWindowSize);
    mSearchWindow->setValue(prof.coarseSearchWindow);
    mFineWindow->setValue(prof.fineWindowSize);
    mOffsetPerBurst->setValue(prof.offsetPerBurst);
    mCorrThreshold->setValue(prof.correlationThreshold);
    int idx = mPolyDegree->findData(prof.polynomialDegree);
    if (idx >= 0) mPolyDegree->setCurrentIndex(idx);
}

// ── 策略解释 (随产品模式/等级/引擎覆盖联动) ──
void RegistrationDialog::updateStrategyView()
{
    mProductModeLabel->setText(QStringLiteral("产品模式: %1 (自动识别)")
        .arg(productModeName(mMetaHolder.productMode)));

    RegistrationStrategy strat = StrategyFactory::create(
        mMetaHolder.productMode, mActiveLevel);
    int engData = mCoarseEngineCombo->currentData().toInt();
    if (engData >= 0) {
        strat.coarseCorr = static_cast<CorrelationMethod>(engData);
        strat.summary.prepend(QStringLiteral("(粗配准引擎已覆盖) "));
    }
    QString text = strat.summary;
    if (!strat.note.isEmpty())
        text += QStringLiteral("\n说明: ") + strat.note;
    mStrategySummary->setText(text);
}

// ── setParams / params ──

void RegistrationDialog::setParams(const RegistrationParams& p)
{
    mMetaHolder = p;
    mActiveLevel = p.level;

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

    // 等级
    QAbstractButton* lvlBtn = mLevelGroup->button(static_cast<int>(p.level));
    if (lvlBtn) lvlBtn->setChecked(true);

    // 引擎覆盖
    int engIdx = mCoarseEngineCombo->findData(
        p.coarseCorrOverride.has_value()
            ? static_cast<int>(p.coarseCorrOverride.value()) : -1);
    if (engIdx >= 0) mCoarseEngineCombo->setCurrentIndex(engIdx);
    else mCoarseEngineCombo->setCurrentIndex(0);

    loadProfile(p.level);

    // 重采样
    int idx = mResamplingMethod->findData(p.resamplingMethod);
    if (idx >= 0) mResamplingMethod->setCurrentIndex(idx);
    mSincWindow->setValue(p.sincWindowSize);
    mSincBeta->setValue(p.sincBeta);

    // 输出
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
    mEstimateBaseline->setChecked(p.estimateBaseline);

    updateStrategyView();
}

RegistrationParams RegistrationDialog::params() const
{
    RegistrationParams p = mMetaHolder;

    saveCurrentProfile();
    p.level = mActiveLevel;

    p.masterPath = mMasterPath->text();
    p.slavePath = mSlavePath->text();

    // 引擎覆盖
    int engData = mCoarseEngineCombo->currentData().toInt();
    if (engData >= 0)
        p.coarseCorrOverride = static_cast<CorrelationMethod>(engData);
    else
        p.coarseCorrOverride.reset();

    // 扁平参数 ← 当前等级 (管线读取)
    const auto& prof = p.profiles[static_cast<int>(p.level)];
    p.coarseWindowSize = prof.coarseWindowSize;
    p.coarseSearchWindow = prof.coarseSearchWindow;
    p.fineWindowSize = prof.fineWindowSize;
    p.offsetPerBurst = prof.offsetPerBurst;
    p.correlationThreshold = prof.correlationThreshold;
    p.polynomialDegree = prof.polynomialDegree;

    // 重采样
    p.resamplingMethod = mResamplingMethod->currentData().toString();
    p.sincWindowSize = mSincWindow->value();
    p.sincBeta = mSincBeta->value();

    // 输出
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    p.estimateBaseline = mEstimateBaseline->isChecked();

    return p;
}
