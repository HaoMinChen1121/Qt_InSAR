#ifndef REGISTRATIONDIALOG_H
#define REGISTRATIONDIALOG_H

#include <QDialog>
#include "domain/params/RegistrationParams.h"

class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;

class RegistrationDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RegistrationDialog(QWidget* parent = nullptr);

    void setParams(const RegistrationParams& p);
    RegistrationParams params() const;

private:
    // Tab 1: 主辅影像
    QLineEdit* mMasterPath;
    QLineEdit* mSlavePath;
    QLabel* mMasterMeta;
    QLabel* mSlaveMeta;

    // Tab 2: 配准
    QComboBox* mCoarseMethod;
    QSpinBox* mControlPoints;
    QSpinBox* mSearchWindow;
    QComboBox* mFineMethod;
    QSpinBox* mWindowSize;
    QDoubleSpinBox* mCorrThreshold;
    QComboBox* mPolyDegree;

    // Tab 3: 重采样
    QComboBox* mResamplingMethod;
    QDoubleSpinBox* mOutResRange;
    QDoubleSpinBox* mOutResAzimuth;
    QSpinBox* mSincWindow;
    QDoubleSpinBox* mSincBeta;

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
    QCheckBox* mEstimateBaseline;
};

#endif // REGISTRATIONDIALOG_H
