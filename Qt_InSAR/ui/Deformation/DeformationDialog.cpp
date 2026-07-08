#include "DeformationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

DeformationDialog::DeformationDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("形变分析参数"));
    setMinimumSize(520, 480);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    QTabWidget* tabs = new QTabWidget(this);
    tabs->addTab(createDeformationTab(), QStringLiteral("形变计算"));
    tabs->addTab(createTimeSeriesTab(), QStringLiteral("时序分析"));
    tabs->addTab(createAtmosphericTab(), QStringLiteral("大气校正"));
    tabs->addTab(createOutputTab(), QStringLiteral("输出设置"));
    mainLayout->addWidget(tabs);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);
}

QWidget* DeformationDialog::createDeformationTab()
{
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    QWidget* w = new QWidget(scroll);
    QVBoxLayout* vl = new QVBoxLayout(w);

    // 转换参数
    QGroupBox* grpConv = new QGroupBox(QStringLiteral("相位转换"), w);
    QFormLayout* formConv = new QFormLayout(grpConv);
    mConversionMode = new QComboBox(grpConv);
    mConversionMode->addItems({QStringLiteral("相位→形变"), QStringLiteral("相位→高程")});
    formConv->addRow(QStringLiteral("转换模式:"), mConversionMode);

    mProjectionDir = new QComboBox(grpConv);
    mProjectionDir->addItems({QStringLiteral("LOS方向"), QStringLiteral("垂直向"), QStringLiteral("水平向")});
    formConv->addRow(QStringLiteral("投影方向:"), mProjectionDir);

    mWavelength = new QDoubleSpinBox(grpConv);
    mWavelength->setDecimals(4); mWavelength->setRange(0.001, 1.0);
    mWavelength->setValue(0.03125); mWavelength->setSuffix(QStringLiteral(" m"));
    formConv->addRow(QStringLiteral("波长 λ:"), mWavelength);

    mIncidenceAngle = new QDoubleSpinBox(grpConv);
    mIncidenceAngle->setRange(0, 90); mIncidenceAngle->setValue(35.0);
    mIncidenceAngle->setSuffix(QStringLiteral(" °"));
    formConv->addRow(QStringLiteral("入射角:"), mIncidenceAngle);

    mSlantRange = new QDoubleSpinBox(grpConv);
    mSlantRange->setDecimals(0); mSlantRange->setRange(1000, 9999999);
    mSlantRange->setValue(850000); mSlantRange->setSuffix(QStringLiteral(" m"));
    formConv->addRow(QStringLiteral("斜距:"), mSlantRange);

    mBaselinePerp = new QDoubleSpinBox(grpConv);
    mBaselinePerp->setDecimals(1); mBaselinePerp->setRange(-9999, 9999);
    mBaselinePerp->setValue(150.0); mBaselinePerp->setSuffix(QStringLiteral(" m"));
    formConv->addRow(QStringLiteral("垂直基线:"), mBaselinePerp);
    vl->addWidget(grpConv);
    vl->addStretch();

    scroll->setWidget(w);
    return scroll;
}

QWidget* DeformationDialog::createTimeSeriesTab()
{
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    QWidget* w = new QWidget(scroll);
    QVBoxLayout* vl = new QVBoxLayout(w);

    QGroupBox* grpTs = new QGroupBox(QStringLiteral("时序分析方法"), w);
    QFormLayout* formTs = new QFormLayout(grpTs);
    mTsMethod = new QComboBox(grpTs);
    mTsMethod->addItems({QStringLiteral("Stacking"), QStringLiteral("SBAS"), QStringLiteral("PS-InSAR")});
    formTs->addRow(QStringLiteral("时序方法:"), mTsMethod);

    mMinTempBaseline = new QSpinBox(grpTs);
    mMinTempBaseline->setRange(1, 3650); mMinTempBaseline->setValue(30);
    mMinTempBaseline->setSuffix(QStringLiteral(" 天"));
    formTs->addRow(QStringLiteral("最小时间基线:"), mMinTempBaseline);

    mMaxSpatialBaseline = new QDoubleSpinBox(grpTs);
    mMaxSpatialBaseline->setDecimals(0); mMaxSpatialBaseline->setRange(1, 5000);
    mMaxSpatialBaseline->setValue(300); mMaxSpatialBaseline->setSuffix(QStringLiteral(" m"));
    formTs->addRow(QStringLiteral("最大空间基线:"), mMaxSpatialBaseline);

    mRefPointPath = new QLineEdit(grpTs);
    formTs->addRow(QStringLiteral("参考点文件:"), mRefPointPath);

    mUnwrapMethod = new QComboBox(grpTs);
    mUnwrapMethod->addItems({QStringLiteral("2D解缠"), QStringLiteral("3D解缠"), QStringLiteral("SNAPHU")});
    formTs->addRow(QStringLiteral("解缠方法:"), mUnwrapMethod);
    vl->addWidget(grpTs);
    vl->addStretch();

    scroll->setWidget(w);
    return scroll;
}

QWidget* DeformationDialog::createAtmosphericTab()
{
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    QWidget* w = new QWidget(scroll);
    QVBoxLayout* vl = new QVBoxLayout(w);

    QGroupBox* grpAtm = new QGroupBox(QStringLiteral("大气相位校正"), w);
    QFormLayout* formAtm = new QFormLayout(grpAtm);
    mAtmMethod = new QComboBox(grpAtm);
    mAtmMethod->addItems({QStringLiteral("GACOS"), QStringLiteral("线性模型"), QStringLiteral("无")});
    formAtm->addRow(QStringLiteral("校正方法:"), mAtmMethod);

    mGacosZtdPath = new QLineEdit(grpAtm);
    formAtm->addRow(QStringLiteral("GACOS ZTD:"), mGacosZtdPath);

    mGacosStdPath = new QLineEdit(grpAtm);
    formAtm->addRow(QStringLiteral("GACOS STD:"), mGacosStdPath);

    mLinearRamp = new QCheckBox(QStringLiteral("线性斜坡估计"), grpAtm);
    mLinearRamp->setChecked(true);
    formAtm->addRow(mLinearRamp);

    mElevationCorr = new QCheckBox(QStringLiteral("高程相关校正"), grpAtm);
    mElevationCorr->setChecked(true);
    formAtm->addRow(mElevationCorr);
    vl->addWidget(grpAtm);
    vl->addStretch();

    scroll->setWidget(w);
    return scroll;
}

QWidget* DeformationDialog::createOutputTab()
{
    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);

    QWidget* w = new QWidget(scroll);
    QVBoxLayout* vl = new QVBoxLayout(w);

    QGroupBox* grpOut = new QGroupBox(QStringLiteral("输出设置"), w);
    QFormLayout* formOut = new QFormLayout(grpOut);
    mOutputDir = new QLineEdit(grpOut);
    formOut->addRow(QStringLiteral("输出目录:"), mOutputDir);

    mOutputPrefix = new QLineEdit(grpOut);
    formOut->addRow(QStringLiteral("输出前缀:"), mOutputPrefix);

    mOutputFormat = new QComboBox(grpOut);
    mOutputFormat->addItems({QStringLiteral("GeoTIFF"), QStringLiteral("ENVI"), QStringLiteral("NetCDF")});
    formOut->addRow(QStringLiteral("输出格式:"), mOutputFormat);
    vl->addWidget(grpOut);

    QGroupBox* grpProducts = new QGroupBox(QStringLiteral("输出产品"), w);
    QVBoxLayout* vlProd = new QVBoxLayout(grpProducts);
    mExportRate = new QCheckBox(QStringLiteral("形变速率图"), grpProducts);
    mExportRate->setChecked(true);
    mExportTS = new QCheckBox(QStringLiteral("时序位移 (CSV)"), grpProducts);
    mExportTS->setChecked(true);
    mExportKml = new QCheckBox(QStringLiteral("Google Earth KML"), grpProducts);
    vlProd->addWidget(mExportRate);
    vlProd->addWidget(mExportTS);
    vlProd->addWidget(mExportKml);
    vl->addWidget(grpProducts);
    vl->addStretch();

    scroll->setWidget(w);
    return scroll;
}
