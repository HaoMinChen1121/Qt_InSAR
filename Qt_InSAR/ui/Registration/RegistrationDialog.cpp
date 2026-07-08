#include "RegistrationDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>

RegistrationDialog::RegistrationDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("影像配准参数"));
    setMinimumSize(600, 450);

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
    mCoarseMethod->addItems({tr("轨道法"), tr("互相干法")});
    form2->addRow(tr("粗配准方法:"), mCoarseMethod);
    mControlPoints = new QSpinBox;
    mControlPoints->setRange(16, 1024);
    mControlPoints->setValue(64);
    form2->addRow(tr("控制点数:"), mControlPoints);
    mFineMethod = new QComboBox;
    mFineMethod->addItems({tr("子像素插值"), tr("过采样")});
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
    tabs->addTab(tab2, tr("配准"));

    // ===== Tab 3: 重采样 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* form3 = new QFormLayout(tab3);
    mResamplingMethod = new QComboBox;
    mResamplingMethod->addItems({"Sinc", tr("双线性"), tr("双三次")});
    form3->addRow(tr("重采样方法:"), mResamplingMethod);
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
    mMasterPath->setText(p.masterPath);
    mSlavePath->setText(p.slavePath);
    mCoarseMethod->setCurrentText(p.coarseMethod);
    mControlPoints->setValue(p.coarseControlPoints);
    mFineMethod->setCurrentText(p.fineMethod);
    mWindowSize->setValue(p.fineWindowSize);
    mCorrThreshold->setValue(p.correlationThreshold);
    mResamplingMethod->setCurrentText(p.resamplingMethod);
    mOutResRange->setValue(p.outputResolutionRange);
    mOutResAzimuth->setValue(p.outputResolutionAzimuth);
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
}

RegistrationParams RegistrationDialog::params() const
{
    RegistrationParams p;
    p.masterPath = mMasterPath->text();
    p.slavePath = mSlavePath->text();
    p.coarseMethod = mCoarseMethod->currentText();
    p.coarseControlPoints = mControlPoints->value();
    p.fineMethod = mFineMethod->currentText();
    p.fineWindowSize = mWindowSize->value();
    p.correlationThreshold = mCorrThreshold->value();
    p.resamplingMethod = mResamplingMethod->currentText();
    p.outputResolutionRange = mOutResRange->value();
    p.outputResolutionAzimuth = mOutResAzimuth->value();
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    return p;
}
