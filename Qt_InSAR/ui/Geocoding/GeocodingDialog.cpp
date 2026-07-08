#include "GeocodingDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QDialogButtonBox>

GeocodingDialog::GeocodingDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("投影转换参数"));
    setMinimumSize(560, 420);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget;

    // ===== Tab 1: 编码方法 =====
    QWidget* tab1 = new QWidget;
    QFormLayout* f1 = new QFormLayout(tab1);
    mMethod = new QComboBox;
    mMethod->addItems({"Range-Doppler", tr("多项式")});
    f1->addRow(tr("编码方法:"), mMethod);
    mPolyOrder = new QSpinBox; mPolyOrder->setRange(1, 5); mPolyOrder->setValue(3);
    f1->addRow(tr("多项式阶数:"), mPolyOrder);
    mDemPath = new QLineEdit;
    f1->addRow(tr("DEM文件:"), mDemPath);
    mTerrainCorr = new QCheckBox(tr("地形校正")); mTerrainCorr->setChecked(true);
    f1->addWidget(mTerrainCorr);
    tabs->addTab(tab1, tr("编码方法"));

    // ===== Tab 2: 坐标系统 =====
    QWidget* tab2 = new QWidget;
    QFormLayout* f2 = new QFormLayout(tab2);
    mEpsg = new QSpinBox; mEpsg->setRange(1024, 32767); mEpsg->setValue(4326);
    f2->addRow(tr("目标EPSG:"), mEpsg);
    mResolution = new QDoubleSpinBox; mResolution->setDecimals(4);
    mResolution->setRange(0, 1000); mResolution->setValue(0);
    mResolution->setSpecialValueText(tr("自动"));
    f2->addRow(tr("分辨率(m):"), mResolution);
    mWest = new QDoubleSpinBox; mWest->setDecimals(6);
    mWest->setRange(-180, 180); mWest->setValue(0);
    f2->addRow(tr("西边界:"), mWest);
    mEast = new QDoubleSpinBox; mEast->setDecimals(6);
    mEast->setRange(-180, 180); mEast->setValue(0);
    f2->addRow(tr("东边界:"), mEast);
    mSouth = new QDoubleSpinBox; mSouth->setDecimals(6);
    mSouth->setRange(-90, 90); mSouth->setValue(0);
    f2->addRow(tr("南边界:"), mSouth);
    mNorth = new QDoubleSpinBox; mNorth->setDecimals(6);
    mNorth->setRange(-90, 90); mNorth->setValue(0);
    f2->addRow(tr("北边界:"), mNorth);
    tabs->addTab(tab2, tr("坐标系统"));

    // ===== Tab 3: 输出 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* f3 = new QFormLayout(tab3);
    mFormat = new QComboBox;
    mFormat->addItems({"GeoTIFF", "ENVI"});
    f3->addRow(tr("输出格式:"), mFormat);
    mOutputDir = new QLineEdit;
    f3->addRow(tr("输出目录:"), mOutputDir);
    mOutputPrefix = new QLineEdit("geocoded");
    f3->addRow(tr("文件前缀:"), mOutputPrefix);
    tabs->addTab(tab3, tr("输出"));

    mainLayout->addWidget(tabs);
    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

void GeocodingDialog::setParams(const GeocodingParams& p)
{
    mMethod->setCurrentText(p.method.contains("Polynomial") ? tr("多项式") : "Range-Doppler");
    mPolyOrder->setValue(p.polynomialOrder);
    mDemPath->setText(p.demPath);
    mTerrainCorr->setChecked(p.terrainCorrection);
    mEpsg->setValue(p.targetEpsg);
    mResolution->setValue(p.outputResolution);
    mWest->setValue(p.outputWest);
    mEast->setValue(p.outputEast);
    mSouth->setValue(p.outputSouth);
    mNorth->setValue(p.outputNorth);
    mFormat->setCurrentText(p.outputFormat);
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
}

GeocodingParams GeocodingDialog::params() const
{
    GeocodingParams p;
    p.method = mMethod->currentText().contains(tr("多项式")) ? "Polynomial" : "RangeDoppler";
    p.polynomialOrder = mPolyOrder->value();
    p.demPath = mDemPath->text();
    p.terrainCorrection = mTerrainCorr->isChecked();
    p.targetEpsg = mEpsg->value();
    p.outputResolution = mResolution->value();
    p.outputWest = mWest->value();
    p.outputEast = mEast->value();
    p.outputSouth = mSouth->value();
    p.outputNorth = mNorth->value();
    p.outputFormat = mFormat->currentText();
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    return p;
}
