#include "RegistrationDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>

RegistrationDialog::RegistrationDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("影像配准参数"));
    setMinimumSize(620, 480);

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

    // ===== Tab 2: 配准 =====
    QWidget* tab2 = new QWidget;
    QFormLayout* form2 = new QFormLayout(tab2);
    mCoarseMethod = new QComboBox;
    mCoarseMethod->addItem(tr("轨道法"), "Orbit");
    mCoarseMethod->addItem(tr("互相关"), "CrossCorrelation");
    form2->addRow(tr("粗配准方法:"), mCoarseMethod);
    mControlPoints = new QSpinBox;
    mControlPoints->setRange(16, 1024);
    mControlPoints->setValue(64);
    form2->addRow(tr("控制点数:"), mControlPoints);
    mSearchWindow = new QSpinBox;
    mSearchWindow->setRange(8, 512);
    mSearchWindow->setValue(64);
    form2->addRow(tr("搜索窗口半径:"), mSearchWindow);
    mFineMethod = new QComboBox;
    mFineMethod->addItem(tr("亚像素"), "SubPixel");
    mFineMethod->addItem(tr("过采样"), "Oversample");
    form2->addRow(tr("精配准方法:"), mFineMethod);
    mWindowSize = new QSpinBox;
    mWindowSize->setRange(8, 256);
    mWindowSize->setValue(32);
    form2->addRow(tr("匹配窗口大小:"), mWindowSize);
    mCorrThreshold = new QDoubleSpinBox;
    mCorrThreshold->setRange(0.0, 1.0);
    mCorrThreshold->setSingleStep(0.05);
    mCorrThreshold->setValue(0.3);
    form2->addRow(tr("相关性阈值:"), mCorrThreshold);
    mPolyDegree = new QComboBox;
    mPolyDegree->addItem("1", 1);
    mPolyDegree->addItem("2", 2);
    mPolyDegree->addItem("3", 3);
    mPolyDegree->setCurrentIndex(1);
    form2->addRow(tr("多项式阶数:"), mPolyDegree);
    tabs->addTab(tab2, tr("配准"));

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
    form3->addRow(tr("Kaiser β:"), mSincBeta);
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
    mEnableEsd = new QCheckBox(tr("ESD方位向精化 (TOPSAR)"));
    mEnableEsd->setChecked(true);
    mEnableEsd->setToolTip(tr("增强频谱分集 — 利用burst重叠区细化方位向偏移至<0.001像素"));
    form4->addRow(mEnableEsd);
    tabs->addTab(tab4, tr("输出"));

    mainLayout->addWidget(tabs);

    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);

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

void RegistrationDialog::setParams(const RegistrationParams& p)
{
    // 保留元数据，防止 params() 输出时丢失
    mMetaHolder = p;

    mMasterPath->setText(p.masterPath);
    mSlavePath->setText(p.slavePath);

    // 更新元数据标签
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

    int idx = mCoarseMethod->findData(p.coarseMethod);
    if (idx >= 0) mCoarseMethod->setCurrentIndex(idx);
    mControlPoints->setValue(p.coarseControlPoints);
    mSearchWindow->setValue(p.coarseSearchWindow);
    idx = mFineMethod->findData(p.fineMethod);
    if (idx >= 0) mFineMethod->setCurrentIndex(idx);
    mWindowSize->setValue(p.fineWindowSize);
    mCorrThreshold->setValue(p.correlationThreshold);
    idx = mPolyDegree->findData(p.polynomialDegree);
    if (idx >= 0) mPolyDegree->setCurrentIndex(idx);
    idx = mResamplingMethod->findData(p.resamplingMethod);
    if (idx >= 0) mResamplingMethod->setCurrentIndex(idx);
    mSincWindow->setValue(p.sincWindowSize);
    mSincBeta->setValue(p.sincBeta);
    mOutResRange->setValue(p.outputResolutionRange);
    mOutResAzimuth->setValue(p.outputResolutionAzimuth);
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
    mEstimateBaseline->setChecked(p.estimateBaseline);
    mEnableEsd->setChecked(p.enableEsd);
}

RegistrationParams RegistrationDialog::params() const
{
    RegistrationParams p = mMetaHolder; // 保留元数据
    p.masterPath = mMasterPath->text();
    p.slavePath = mSlavePath->text();
    p.coarseMethod = mCoarseMethod->currentData().toString();
    p.coarseControlPoints = mControlPoints->value();
    p.coarseSearchWindow = mSearchWindow->value();
    p.fineMethod = mFineMethod->currentData().toString();
    p.fineWindowSize = mWindowSize->value();
    p.correlationThreshold = mCorrThreshold->value();
    p.polynomialDegree = mPolyDegree->currentData().toInt();
    p.resamplingMethod = mResamplingMethod->currentData().toString();
    p.sincWindowSize = mSincWindow->value();
    p.sincBeta = mSincBeta->value();
    p.outputResolutionRange = mOutResRange->value();
    p.outputResolutionAzimuth = mOutResAzimuth->value();
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    p.estimateBaseline = mEstimateBaseline->isChecked();
    p.enableEsd = mEnableEsd->isChecked();
    return p;
}
