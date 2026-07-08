#include "FilterUnwrappingDialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QGroupBox>

FilterUnwrappingDialog::FilterUnwrappingDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("滤波与解缠参数"));
    setMinimumSize(580, 450);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QTabWidget* tabs = new QTabWidget;

    // ===== Tab 1: 滤波 =====
    QWidget* tab1 = new QWidget;
    QFormLayout* f1 = new QFormLayout(tab1);
    mFilterMethod = new QComboBox;
    mFilterMethod->addItems({"Goldstein & Werner", "Baran et al."});
    f1->addRow(tr("滤波方法:"), mFilterMethod);
    mAlpha = new QDoubleSpinBox; mAlpha->setRange(0.1, 1.0);
    mAlpha->setSingleStep(0.1); mAlpha->setValue(0.5);
    f1->addRow(tr("Alpha:"), mAlpha);
    mWindowSize = new QSpinBox; mWindowSize->setRange(8, 256);
    mWindowSize->setSingleStep(8); mWindowSize->setValue(32);
    f1->addRow(tr("窗口大小:"), mWindowSize);
    mPatchSize = new QSpinBox; mPatchSize->setRange(16, 512);
    mPatchSize->setSingleStep(16); mPatchSize->setValue(64);
    f1->addRow(tr("Patch大小:"), mPatchSize);
    mIterations = new QSpinBox; mIterations->setRange(1, 20); mIterations->setValue(3);
    f1->addRow(tr("迭代次数:"), mIterations);
    tabs->addTab(tab1, tr("滤波"));

    // ===== Tab 2: 解缠 =====
    QWidget* tab2 = new QWidget;
    QFormLayout* f2 = new QFormLayout(tab2);
    mUnwrapMethod = new QComboBox;
    mUnwrapMethod->addItems({tr("枝切法"), tr("最小二乘法")});
    f2->addRow(tr("解缠方法:"), mUnwrapMethod);
    mCohThreshold = new QDoubleSpinBox; mCohThreshold->setRange(0, 1);
    mCohThreshold->setSingleStep(0.05); mCohThreshold->setValue(0.3);
    f2->addRow(tr("相干阈值:"), mCohThreshold);
    mMaskPath = new QLineEdit; mMaskPath->setPlaceholderText(tr("掩膜文件(可选)"));
    f2->addRow(tr("掩膜文件:"), mMaskPath);
    mMinRegion = new QSpinBox; mMinRegion->setRange(1, 10000); mMinRegion->setValue(100);
    f2->addRow(tr("最小区域(像素):"), mMinRegion);
    mMaxResidues = new QSpinBox; mMaxResidues->setRange(10, 50000); mMaxResidues->setValue(500);
    f2->addRow(tr("最大残差点数:"), mMaxResidues);
    mWeightedLS = new QCheckBox(tr("加权最小二乘")); mWeightedLS->setChecked(true);
    f2->addWidget(mWeightedLS);
    mMaxIterations = new QSpinBox; mMaxIterations->setRange(10, 10000); mMaxIterations->setValue(1000);
    f2->addRow(tr("最大迭代:"), mMaxIterations);
    mConvergeTol = new QDoubleSpinBox; mConvergeTol->setDecimals(6);
    mConvergeTol->setRange(1e-8, 0.1); mConvergeTol->setValue(1e-4);
    f2->addRow(tr("收敛阈值:"), mConvergeTol);
    tabs->addTab(tab2, tr("解缠"));

    // ===== Tab 3: 相位高程 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* f3 = new QFormLayout(tab3);
    mConvertHeight = new QCheckBox(tr("将解缠相位转换为高程"));
    f3->addWidget(mConvertHeight);
    mWavelength = new QDoubleSpinBox; mWavelength->setDecimals(6);
    mWavelength->setRange(0.001, 1.0); mWavelength->setValue(0.03125);
    f3->addRow(tr("雷达波长(m):"), mWavelength);
    mIncAngle = new QDoubleSpinBox; mIncAngle->setRange(0, 90);
    mIncAngle->setDecimals(2); mIncAngle->setValue(35.0);
    f3->addRow(tr("入射角(°):"), mIncAngle);
    mSlantRange = new QDoubleSpinBox; mSlantRange->setRange(1000, 10000000);
    mSlantRange->setDecimals(0); mSlantRange->setValue(800000);
    f3->addRow(tr("斜距(m):"), mSlantRange);
    mBaselinePerp = new QDoubleSpinBox; mBaselinePerp->setRange(1, 10000);
    mBaselinePerp->setDecimals(1); mBaselinePerp->setValue(200);
    f3->addRow(tr("垂直基线(m):"), mBaselinePerp);
    tabs->addTab(tab3, tr("相位高程"));

    // ===== Tab 4: 输出 =====
    QWidget* tab4 = new QWidget;
    QFormLayout* f4 = new QFormLayout(tab4);
    mOutputDir = new QLineEdit;
    f4->addRow(tr("输出目录:"), mOutputDir);
    mOutputPrefix = new QLineEdit("filtered_unwrapped");
    f4->addRow(tr("文件前缀:"), mOutputPrefix);
    tabs->addTab(tab4, tr("输出"));

    mainLayout->addWidget(tabs);
    QDialogButtonBox* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

void FilterUnwrappingDialog::setFilterParams(const FilterParams& p)
{
    mFilterMethod->setCurrentText(p.method.contains("Baran") ? "Baran et al." : "Goldstein & Werner");
    mAlpha->setValue(p.goldsteinAlpha);
    mWindowSize->setValue(p.goldsteinWindowSize);
    mPatchSize->setValue(p.goldsteinPatchSize);
    mIterations->setValue(p.baranIterations);
}
FilterParams FilterUnwrappingDialog::filterParams() const
{
    FilterParams p;
    p.method = mFilterMethod->currentText().contains("Baran") ? "Baran" : "Goldstein";
    p.goldsteinAlpha = mAlpha->value(); p.baranAlpha = mAlpha->value();
    p.goldsteinWindowSize = mWindowSize->value(); p.baranWindowSize = mWindowSize->value();
    p.goldsteinPatchSize = mPatchSize->value();
    p.baranIterations = mIterations->value();
    return p;
}

void FilterUnwrappingDialog::setUnwrappingParams(const UnwrappingParams& p)
{
    mUnwrapMethod->setCurrentText(p.method.contains("LeastSquares") ? tr("最小二乘法") : tr("枝切法"));
    mCohThreshold->setValue(p.coherenceThreshold);
    mMaskPath->setText(p.maskPath);
    mMinRegion->setValue(p.minRegionSize);
    mMaxResidues->setValue(p.branchCutMaxResidues);
    mWeightedLS->setChecked(p.useWeightedLeastSquares);
    mMaxIterations->setValue(p.maxIterations);
    mConvergeTol->setValue(p.convergenceTolerance);
    mConvertHeight->setChecked(p.convertToHeight);
    mWavelength->setValue(p.wavelength);
    mIncAngle->setValue(p.incidenceAngle);
    mSlantRange->setValue(p.slantRange);
    mBaselinePerp->setValue(p.baselinePerp);
}
UnwrappingParams FilterUnwrappingDialog::unwrappingParams() const
{
    UnwrappingParams p;
    p.method = mUnwrapMethod->currentText().contains(tr("最小二乘")) ? "LeastSquares" : "BranchCut";
    p.coherenceThreshold = mCohThreshold->value();
    p.maskPath = mMaskPath->text();
    p.minRegionSize = mMinRegion->value();
    p.branchCutMaxResidues = mMaxResidues->value();
    p.useWeightedLeastSquares = mWeightedLS->isChecked();
    p.maxIterations = mMaxIterations->value();
    p.convergenceTolerance = mConvergeTol->value();
    p.convertToHeight = mConvertHeight->isChecked();
    p.wavelength = mWavelength->value();
    p.incidenceAngle = mIncAngle->value();
    p.slantRange = mSlantRange->value();
    p.baselinePerp = mBaselinePerp->value();
    return p;
}
