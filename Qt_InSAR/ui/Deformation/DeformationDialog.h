#ifndef DEFORMATIONDIALOG_H
#define DEFORMATIONDIALOG_H

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QLineEdit;
class QLabel;

class DeformationDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DeformationDialog(QWidget* parent = nullptr);

private:
    QWidget* createDeformationTab();
    QWidget* createTimeSeriesTab();
    QWidget* createAtmosphericTab();
    QWidget* createOutputTab();

    // Tab 1: 形变计算
    QComboBox* mConversionMode;
    QComboBox* mProjectionDir;
    QDoubleSpinBox* mIncidenceAngle;
    QDoubleSpinBox* mWavelength;
    QDoubleSpinBox* mSlantRange;
    QDoubleSpinBox* mBaselinePerp;

    // Tab 2: 时序分析
    QComboBox* mTsMethod;
    QSpinBox* mMinTempBaseline;
    QDoubleSpinBox* mMaxSpatialBaseline;
    QLineEdit* mRefPointPath;
    QComboBox* mUnwrapMethod;

    // Tab 3: 大气校正
    QComboBox* mAtmMethod;
    QLineEdit* mGacosZtdPath;
    QLineEdit* mGacosStdPath;
    QCheckBox* mLinearRamp;
    QCheckBox* mElevationCorr;

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
    QComboBox* mOutputFormat;
    QCheckBox* mExportRate;
    QCheckBox* mExportTS;
    QCheckBox* mExportKml;
};

#endif // DEFORMATIONDIALOG_H
