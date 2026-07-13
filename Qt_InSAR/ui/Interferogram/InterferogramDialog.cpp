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
            mCachedIncAngle = 35.0;
            mIncAngleLabel->setText(QStringLiteral("入射角: (未加载产品)"));
            mIncAngleLabel->setStyleSheet("color: #888;");
            return;
        }

        auto readIncFromXml = [](const QString& prodPath) -> double {
            QScopedPointer<ISarProduct> prod(createSarProduct(prodPath));
            if (!prod || !prod->open(prodPath)) return 0;
            const auto& bands = prod->bands();
            if (bands.isEmpty()) return 0;
            // 从波段 VSI 路径推导 annotation 目录
            QString bandPath = bands.first().rasterPath;
            int measIdx = bandPath.lastIndexOf("/measurement/");
            if (measIdx < 0) return 0;
            QString annDir = bandPath.left(measIdx) + "/annotation";
            char** entries = VSIReadDir(annDir.toUtf8().constData());
            if (!entries) return 0;
            double result = 0;
            for (int i = 0; entries[i] && result < 1.0; ++i) {
                QString e = QString::fromUtf8(entries[i]);
                if (!e.endsWith(".xml") || e.contains("calibration")) continue;
                VSILFILE* fp = VSIFOpenExL((annDir + "/" + e).toUtf8().constData(), "rb", TRUE);
                if (!fp) continue;
                QByteArray xml; xml.resize(1024*1024);
                vsi_l_offset n = VSIFReadL(xml.data(), 1, xml.size(), fp);
                xml.resize(static_cast<int>(n)); VSIFCloseL(fp);
                // 用 QDomDocument 正确解析（自动处理 UTF-8/16 编码）
                QDomDocument doc;
                if (!doc.setContent(xml)) continue;  // 自动检测编码
                QDomNodeList nl2 = doc.elementsByTagName("incidenceAngleMidSwath");
                if (!nl2.isEmpty()) {
                    result = QLocale::c().toDouble(nl2.at(0).toElement().text().trimmed());
                }
            }
            CSLDestroy(entries);
            return result;
        };

        mCachedIncAngle = readIncFromXml(path);
        if (mCachedIncAngle < 1.0) mCachedIncAngle = 35.0;

        mIncAngleLabel->setText(QStringLiteral("入射角: %1° (从产品读取)").arg(mCachedIncAngle, 0, 'f', 1));
        mIncAngleLabel->setStyleSheet("color: #27AE60; font-weight: bold;");
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
    mOutputType = new QComboBox;
    mOutputType->addItems({tr("复数"), tr("相位"), tr("相干性")});
    f1->addRow(tr("输出类型:"), mOutputType);
    mSpectralFilter = new QCheckBox(tr("频谱偏移滤波")); mSpectralFilter->setChecked(true);
    f1->addWidget(mSpectralFilter);
    tabs->addTab(tab1, tr("干涉图"));

    // ===== Tab 2: 去平地 =====
    QWidget* tab2 = new QWidget;
    QFormLayout* f2 = new QFormLayout(tab2);
    mRefSource = new QComboBox;
    mRefSource->addItems({tr("参考椭球"), tr("轨道数据"), tr("外部DEM")});
    f2->addRow(tr("参考源:"), mRefSource);
    mSemiMajor = new QDoubleSpinBox; mSemiMajor->setDecimals(1);
    mSemiMajor->setRange(6370000, 6380000); mSemiMajor->setValue(6378137.0);
    f2->addRow(tr("长半轴(m):"), mSemiMajor);
    mEccentricity = new QDoubleSpinBox; mEccentricity->setDecimals(8);
    mEccentricity->setRange(0, 1); mEccentricity->setValue(0.00669438);
    f2->addRow(tr("偏心率:"), mEccentricity);
    mOrbitFile = new QLineEdit;
    f2->addRow(tr("轨道文件:"), mOrbitFile);
    mFlatDemPath = new QLineEdit;
    QPushButton* flatDemBrowse = new QPushButton(tr("浏览..."));
    QHBoxLayout* flatDemLayout = new QHBoxLayout;
    flatDemLayout->addWidget(mFlatDemPath, 1); flatDemLayout->addWidget(flatDemBrowse);
    f2->addRow(tr("DEM文件:"), flatDemLayout);
    connect(flatDemBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, tr("选择DEM文件"),
            QString(), tr("DEM (*.tif *.tiff *.dem *.img);;所有 (*.*)"));
        if (!f.isEmpty()) mFlatDemPath->setText(f);
    });
    mPreciseOrbit = new QCheckBox(tr("使用精密轨道")); mPreciseOrbit->setChecked(true);
    f2->addWidget(mPreciseOrbit);
    mIncAngleLabel = new QLabel(tr("入射角: 35.0° (从主产品自动获取)"), this);
    f2->addWidget(mIncAngleLabel);
    tabs->addTab(tab2, tr("去平地"));

    // ===== Tab 3: 差分 =====
    QWidget* tab3 = new QWidget;
    QFormLayout* f3 = new QFormLayout(tab3);
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
    mDispDirection = new QComboBox;
    mDispDirection->addItems({"LOS", tr("垂直向")});
    f3->addRow(tr("形变方向:"), mDispDirection);
    mAtmCorr = new QCheckBox(tr("大气校正"));
    f3->addWidget(mAtmCorr);
    mAtmModel = new QComboBox;
    mAtmModel->addItems({tr("线性模型"), tr("幂律模型")});
    f3->addRow(tr("大气模型:"), mAtmModel);
    mTopoCorr = new QCheckBox(tr("地形相位去除")); mTopoCorr->setChecked(true);
    f3->addWidget(mTopoCorr);
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
    mCachedIncAngle = p.incidenceAngle;

    // 从主产品读取真实入射角
    if (!p.masterQsarPath.isEmpty()) {
        QScopedPointer<ISarProduct> prod(createSarProduct(p.masterQsarPath));
        if (prod && prod->open(p.masterQsarPath))
            mCachedIncAngle = prod->sensorInfo().incidenceAngleMid;
    }

    mMasterQsar->setText(p.masterQsarPath);
    mSlaveQsar->setText(p.slaveQsarPath);
    mRangeLooks->setValue(p.rangeLooks);
    mAzimuthLooks->setValue(p.azimuthLooks);
    mOutputType->setCurrentText(p.outputType);
    mSpectralFilter->setChecked(p.spectralFilter);
    mRefSource->setCurrentText(p.referenceSource);
    mDiffDemPath->setText(p.demPath);
    mDispDirection->setCurrentText(p.displacementDirection);
    mAtmCorr->setChecked(p.atmosphericCorrection);
    mIncAngleLabel->setText(QStringLiteral("入射角: %1° (从主产品自动获取)").arg(mCachedIncAngle, 0, 'f', 1));
    mOutputDir->setText(p.outputDir);
    mOutputPrefix->setText(p.outputPrefix);
}

InterferogramParams InterferogramDialog::params() const
{
    InterferogramParams p;
    p.masterQsarPath = mMasterQsar->text();
    p.slaveQsarPath = mSlaveQsar->text();
    p.rangeLooks = mRangeLooks->value();
    p.azimuthLooks = mAzimuthLooks->value();
    p.outputType = mOutputType->currentText();
    p.spectralFilter = mSpectralFilter->isChecked();
    p.referenceSource = mRefSource->currentText();
    p.demPath = mDiffDemPath->text();
    p.displacementDirection = mDispDirection->currentText();
    p.atmosphericCorrection = mAtmCorr->isChecked();
    p.enableDifferential = mTopoCorr->isChecked();
    p.incidenceAngle = mCachedIncAngle;
    p.outputDir = mOutputDir->text();
    p.outputPrefix = mOutputPrefix->text();
    return p;
}

void InterferogramDialog::setFlatEarthParams(const FlatEarthParams& p)
{
    mRefSource->setCurrentText(p.method);
    mSemiMajor->setValue(p.semiMajorAxis);
    mEccentricity->setValue(p.eccentricity);
    mOrbitFile->setText(p.orbitFilePath);
    mFlatDemPath->setText(p.demPath);
    mPreciseOrbit->setChecked(p.usePreciseOrbit);
}

FlatEarthParams InterferogramDialog::flatEarthParams() const
{
    FlatEarthParams p;
    p.method = mRefSource->currentText();
    p.semiMajorAxis = mSemiMajor->value();
    p.eccentricity = mEccentricity->value();
    p.orbitFilePath = mOrbitFile->text();
    p.demPath = mFlatDemPath->text();
    p.usePreciseOrbit = mPreciseOrbit->isChecked();
    return p;
}

void InterferogramDialog::setDifferentialParams(const DifferentialParams& p)
{
    mDiffDemPath->setText(p.demPath);
    mDispDirection->setCurrentText(p.displacementDirection);
    mAtmCorr->setChecked(p.atmosphericCorrection);
    mAtmModel->setCurrentText(p.atmosphericModel);
    mTopoCorr->setChecked(p.topographicCorrection);
}

DifferentialParams InterferogramDialog::differentialParams() const
{
    DifferentialParams p;
    p.demPath = mDiffDemPath->text();
    p.displacementDirection = mDispDirection->currentText();
    p.atmosphericCorrection = mAtmCorr->isChecked();
    p.atmosphericModel = mAtmModel->currentText();
    p.topographicCorrection = mTopoCorr->isChecked();
    return p;
}
