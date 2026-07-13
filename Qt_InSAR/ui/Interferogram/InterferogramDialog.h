#ifndef INTERFEROGRAMDIALOG_H
#define INTERFEROGRAMDIALOG_H

#include <QDialog>
#include "domain/params/InterferogramParams.h"
#include "domain/params/FlatEarthParams.h"
#include "domain/params/DifferentialParams.h"

class QSpinBox;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QDoubleSpinBox;
class QLabel;

class InterferogramDialog : public QDialog
{
    Q_OBJECT
public:
    explicit InterferogramDialog(QWidget* parent = nullptr);
    void setParams(const InterferogramParams& p);
    InterferogramParams params() const;

    void setFlatEarthParams(const FlatEarthParams& p);
    FlatEarthParams flatEarthParams() const;

    void setDifferentialParams(const DifferentialParams& p);
    DifferentialParams differentialParams() const;

private:
    // Tab 0: 输入
    QLineEdit* mMasterQsar;
    QLineEdit* mSlaveQsar;

    // Tab 1: 干涉图
    QSpinBox* mRangeLooks;
    QSpinBox* mAzimuthLooks;
    QComboBox* mOutputType;
    QCheckBox* mSpectralFilter;

    // Tab 2: 去平地
    QComboBox* mRefSource;
    QDoubleSpinBox* mSemiMajor;
    QDoubleSpinBox* mEccentricity;
    QLineEdit* mOrbitFile;
    QLineEdit* mFlatDemPath;
    QCheckBox* mPreciseOrbit;
    QLabel*    mIncAngleLabel;
    double     mCachedIncAngle = 35.0;

    // Tab 3: 差分
    QLineEdit* mDiffDemPath;
    QComboBox* mDispDirection;
    QCheckBox* mAtmCorr;
    QComboBox* mAtmModel;
    QCheckBox* mTopoCorr;

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
};

#endif // INTERFEROGRAMDIALOG_H
