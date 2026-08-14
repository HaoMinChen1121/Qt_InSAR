#include "InterferogramDialog.h"
#include "dataaccess/SarProductFactory.h"

#include <gdal_priv.h>
#include <cpl_vsi.h>

#include <QVBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QScopedPointer>
#include <QLocale>
#include <QDomDocument>
#include <QHBoxLayout>

InterferogramDialog::InterferogramDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("干涉处理参数"));
    setMinimumSize(600, 450);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget;

    // ===== Tab 0: 输入 =====
    QWidget* tab0 = new QWidget;
    QFormLayout* f0 = new QFormLayout(tab0);
    mMasterQsar = new QLineEdit;
    QPushButton* masterBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* masterLayout = new QHBoxLayout;
    masterLayout->addWidget(mMasterQsar, 1); masterLayout->addWidget(masterBrowse);
    f0->addRow(tr("主影像(zip/SAFE):"), masterLayout);
    auto updateIncAngle = [this]() {
        QString path = mMasterQsar->text().trimmed();
        if (path.isEmpty()) {
            mCachedIncAngle = 0.0;
            mCachedWavelength = 0.0;
            mCachedNearRange = 0.0;
            mCachedRangeSpacing = 0.0;
            mCachedPrf = 0.0;
            mIncAngleLabel->setText(QStringLiteral("入射角: (未加载产品)"));
            mIncAngleLabel->setStyleSheet("color: #888;");
            return;
        }

        QScopedPointer<ISarProduct> prod(createSarProduct(path));
        if (prod && prod->open(path)) {
            SarSensorInfo si = prod->sensorInfo();
            mCachedIncAngle     = si.incidenceAngleMid;
            mCachedWavelength   = si.wavelength;
            mCachedNearRange    = si.nearRange;
            mCachedRangeSpacing = si.rangeSpacing;
            mCachedPrf          = si.prf;
        } else {
            mCachedIncAngle = 0.0;
        }

        if (mCachedIncAngle > 1.0) {
            mIncAngleLabel->setText(QStringLiteral("入射角: %1° (从产品读取)").arg(mCachedIncAngle, 0, 'f', 2));
            mIncAngleLabel->setStyleSheet("color: #27AE60; font-weight: bold;");
        } else {
            mIncAngleLabel->setText(QStringLiteral("入射角: (未从产品获取)"));
            mIncAngleLabel->setStyleSheet("color: #888;");
        }
    };

    connect(masterBrowse, &QPushButton::clicked, this, [this, updateIncAngle]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择主影像产品"),
            QString(), tr("Sentinel-1 (*.zip *.SAFE);;所有 (*.*)"));
        if (!f.isEmpty()) { mMasterQsar->setText(f); updateIncAngle(); }
    });
    connect(mMasterQsar, &QLineEdit::editingFinished, this, updateIncAngle);
    mSlaveQsar = new QLineEdit;
    QPushButton* slaveBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* slaveLayout = new QHBoxLayout;
    slaveLayout->addWidget(mSlaveQsar, 1); slaveLayout->addWidget(slaveBrowse);
    f0->addRow(tr("辅影像(.qsar):"), slaveLayout);
    connect(slaveBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择辅影像QSAR"),
            QString(), tr("QSAR (*.qsar);;所有 (*.*)"));
        if (!f.isEmpty()) mSlaveQsar->setText(f);
    });
    tabs->addTab(tab0, tr("输入"));

    // ===== Tab 1: 干涉图 =====
    QWidget* tab1 = new QWidget;
    QFormLayout* f1 = new QFormLayout(tab1);
    mRangeLooks = new QSpinBox; mRangeLooks->setRange(1, 32); mRangeLooks->setValue(4);
    f1->addRow(tr("距离向多视比:"), mRangeLooks);
    mAzimuthLooks = new QSpinBox; mAzimuthLooks->setRange(1, 32); mAzimuthLooks->setValue(4);
    f1->addRow(tr("方位向多视比:"), mAzimuthLooks);
    mAzRampCorr = new QCheckBox(tr("deburst 方位时间校正"));
    mAzRampCorr->setChecked(true);
    mAzRampCorr->setToolTip(tr("去除逐 burst 残余方位斜坡, 保证 burst 拼接处相位连续 (TOPSAR)"));
    f1->addWidget(mAzRampCorr);
    mPhaseAlign = new QCheckBox(tr("IW 拼接相位一致性对齐"));
    mPhaseAlign->setChecked(true);
    mPhaseAlign->setToolTip(tr("合并产品中子条带重叠区鲁棒估计常数+线性相位差并校正, 消除边界相位台阶"));
    f1->addWidget(mPhaseAlign);
    mVisColor = new QCheckBox(tr("生成 HSV 彩色渲染 (默认展示)"));
    mVisColor->setChecked(true);
    mVisColor->setToolTip(tr("visualization/ 下生成彩色 GeoTIFF: 色相=相位, 饱和度=相干性, 亮度=幅度"));
    f1->addWidget(mVisColor);
    tabs->addTab(tab1, tr("干涉图"));

    // ===== Tab 2: 去平地 =====
    QWidget* tab2 = new QWidget;
    QFormLayout* f2 = new QFormLayout(tab2);
    mEnableFlat = new QCheckBox(tr("启用去平地 (合并产品逐列几何)"));
    mEnableFlat->setChecked(true);
    mEnableFlat->setToolTip(tr("φ_flat = −4π/λ·B∥·sinθ(R), R/θ 由几何表逐列计算"));
    f2->addWidget(mEnableFlat);
    mIncAngleLabel = new QLabel(tr("入射角: (选择主产品后自动获取)"), this);
    f2->addWidget(mIncAngleLabel);
    tabs->addTab(tab2, tr("去平地"));

    // ===== Tab 3: 差分 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* f3 = new QFormLayout(tab3);
    mEnableDiff = new QCheckBox(tr("启用差分 (DEM 地形相位去除)"));
    mEnableDiff->setChecked(true);
    f3->addWidget(mEnableDiff);
    mDiffDemPath = new QLineEdit;
    QPushButton* demBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* demLayout = new QHBoxLayout;
    demLayout->addWidget(mDiffDemPath, 1); demLayout->addWidget(demBrowse);
    f3->addRow(tr("DEM文件:"), demLayout);
    connect(demBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择DEM文件"),
            QString(), tr("DEM (*.tif *.tiff *.dem *.img);;所有 (*.*)"));
        if (!f.isEmpty()) mDiffDemPath->setText(f);
    });
    tabs->addTab(tab3, tr("差分"));

    // ===== Tab 4: 输出 =====
    QWidget* tab4 = new QWidget;
    QFormLayout* f4 = new QFormLayout(tab4);
    mOutputDir = new QLineEdit;
    QPushButton* outDirBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* outDirLayout = new QHBoxLayout;
    outDirLayout->addWidget(mOutputDir, 1); outDirLayout->addWidget(outDirBrowse);
    f4->addRow(tr("输出目录:"), outDirLayout);
    connect(outDirBrowse, &QPushButton::clicked, this, [this]() {
        QString d = QFileDialog::getExistingDirectory(this, tr("选择输出目录"));
        if (!d.isEmpty()) mOutputDir->setText(d);
    });
    mOutputPrefix = new QLineEdit("interferogram");
    f4->addRow(tr("文件前缀:"), mOutputPrefix);
    mAutoLoad = new QCheckBox(tr("完成后自动加载可见图层"));
    mAutoLoad->setChecked(true);
    f4->addWidget(mAutoLoad);
    mLegacyPerIw = new QCheckBox(tr("保留逐子条带中间输出 (兼容旧流程)"));
    mLegacyPerIw->setChecked(false);
    mLegacyPerIw->setToolTip(tr("从合并产品按列切片生成 legacy_iw/ 下的逐 IW flat/diff (边缘缺重叠列)"));
    f4->addWidget(mLegacyPerIw);
    tabs->addTab(tab4, tr("输出"));

    mainLayout->addWidget(tabs);
    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

void InterferogramDialog::setParams(const InterferogramParams& p)
{
    mCachedIncAngle     = p.incidenceAngle;
    mCachedWavelength   = p.wavelength;
    mCachedNearRange    = p.nearRange;
    mCachedRangeSpacing = p.rangeSpacing;
    mCachedPrf          = p.prf;

    mMasterQsar->setText(p.masterQsarPath);
    mSlaveQsar->setText(p.slaveQsarPath);
    mRangeLooks->setValue(p.rangeLooks);
    mAzimuthLooks->setValue(p.azimuthLooks);
    mAzRampCorr->setChecked(p.enableAzimuthRampCorrection);
    mPhaseAlign->setChecked(p.phaseAlign);
    mVisColor->setChecked(p.enableVisualization);
    mEnableFlat->setChecked(p.enableFlatEarth);
    mDiffDemPath->setText(p.demPath);
    mEnableDiff->setChecked(p.enableDifferential);
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
    mLegacyPerIw->setChecked(p.legacyPerIwOutputs);
    mAutoLoad->setChecked(p.autoLoadToCanvas);

    // 入射角显示由 updateIncAngle 回调处理 (editingFinished / browse)
    if (p.masterQsarPath.isEmpty())
        mIncAngleLabel->setText(QStringLiteral("入射角: (未加载产品)"));
    // 非空时 mMasterQsar->setText 会触发 editingFinished → updateIncAngle
}

InterferogramParams InterferogramDialog::params() const
{
    InterferogramParams p;
    p.masterQsarPath = mMasterQsar->text();
    p.slaveQsarPath = mSlaveQsar->text();
    p.rangeLooks = mRangeLooks->value();
    p.azimuthLooks = mAzimuthLooks->value();
    p.enableAzimuthRampCorrection = mAzRampCorr->isChecked();
    p.phaseAlign = mPhaseAlign->isChecked();
    p.enableVisualization = mVisColor->isChecked();
    p.enableFlatEarth = mEnableFlat->isChecked();
    p.demPath = mDiffDemPath->text();
    p.enableDifferential = mEnableDiff->isChecked();
    p.incidenceAngle = mCachedIncAngle;
    p.wavelength     = mCachedWavelength;
    p.nearRange      = mCachedNearRange;
    p.rangeSpacing   = mCachedRangeSpacing;
    p.prf            = mCachedPrf;
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    p.legacyPerIwOutputs = mLegacyPerIw->isChecked();
    p.autoLoadToCanvas = mAutoLoad->isChecked();
    return p;
}
