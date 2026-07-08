#ifndef FILTERUNWRAPPINGDIALOG_H
#define FILTERUNWRAPPINGDIALOG_H

#include <QDialog>
#include "domain/params/FilterParams.h"
#include "domain/params/UnwrappingParams.h"

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QLineEdit;

class FilterUnwrappingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FilterUnwrappingDialog(QWidget* parent = nullptr);

    void setFilterParams(const FilterParams& p);
    FilterParams filterParams() const;

    void setUnwrappingParams(const UnwrappingParams& p);
    UnwrappingParams unwrappingParams() const;

private:
    // Tab 1: 滤波
    QComboBox* mFilterMethod;
    QDoubleSpinBox* mAlpha;
    QSpinBox* mWindowSize;
    QSpinBox* mPatchSize;
    QSpinBox* mIterations;

    // Tab 2: 解缠
    QComboBox* mUnwrapMethod;
    QDoubleSpinBox* mCohThreshold;
    QLineEdit* mMaskPath;
    QSpinBox* mMinRegion;
    QSpinBox* mMaxResidues;
    QCheckBox* mWeightedLS;
    QSpinBox* mMaxIterations;
    QDoubleSpinBox* mConvergeTol;

    // Tab 3: 相位高程
    QCheckBox* mConvertHeight;
    QDoubleSpinBox* mWavelength;
    QDoubleSpinBox* mIncAngle;
    QDoubleSpinBox* mSlantRange;
    QDoubleSpinBox* mBaselinePerp;

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
};

#endif // FILTERUNWRAPPINGDIALOG_H
