#include "FilterUnwrappingDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QFileDialog>
#include <QPushButton>

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

    // ===== Tab 3: 输出 =====
    QWidget* tab4 = new QWidget;
    QFormLayout* f4 = new QFormLayout(tab4);
    QWidget* dirRow = new QWidget(tab4);
    QHBoxLayout* dirLay = new QHBoxLayout(dirRow);
    dirLay->setContentsMargins(0, 0, 0, 0);
    mOutputDir = new QLineEdit(dirRow);
    mOutputDir->setPlaceholderText(tr("留空 = 干涉产品目录下 filter/ 子目录"));
    dirLay->addWidget(mOutputDir, 1);
    QPushButton* btnBrowse = new QPushButton(tr("浏览..."), dirRow);
    dirLay->addWidget(btnBrowse);
    connect(btnBrowse, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择滤波输出目录"), mOutputDir->text());
        if (!dir.isEmpty()) mOutputDir->setText(dir);
    });
    f4->addRow(tr("输出目录:"), dirRow);
    mOutputPrefix = new QLineEdit("ifg_filtered");
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
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
}
FilterParams FilterUnwrappingDialog::filterParams() const
{
    FilterParams p;
    p.method = mFilterMethod->currentText().contains("Baran") ? "Baran" : "Goldstein";
    p.goldsteinAlpha = mAlpha->value(); p.baranAlpha = mAlpha->value();
    p.goldsteinWindowSize = mWindowSize->value(); p.baranWindowSize = mWindowSize->value();
    p.goldsteinPatchSize = mPatchSize->value();
    p.baranIterations = mIterations->value();
    p.outputDir = mOutputDir->text().trimmed();
    p.outputPrefix = mOutputPrefix->text().trimmed();
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
    return p;
}
