#ifndef REGISTRATIONDIALOG_H
#define REGISTRATIONDIALOG_H

#include <QDialog>
#include <QButtonGroup>
#include "domain/params/RegistrationParams.h"

class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QRadioButton;

class RegistrationDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RegistrationDialog(QWidget* parent = nullptr);

    void setParams(const RegistrationParams& p);
    RegistrationParams params() const;

private slots:
    void onLevelChanged(int levelIdx);

private:
    void saveCurrentProfile() const;
    void loadProfile(ProcessingLevel level);
    void updateStrategyView();

    // 保留元数据 (路径/轨道/传感器)，不被对话框覆盖
    // mutable: params() 为 const, 但需要把 UI 值写回当前等级 profile
    mutable RegistrationParams mMetaHolder;
    ProcessingLevel mActiveLevel = ProcessingLevel::Standard;

    // Tab 1: 主辅影像
    QLineEdit* mMasterPath;
    QLineEdit* mSlavePath;
    QLabel* mMasterMeta;
    QLabel* mSlaveMeta;

    // Tab 2: 策略
    QLabel* mProductModeLabel;       // 产品模式 (自动识别, 只读)
    QButtonGroup* mLevelGroup;
    QRadioButton* mFastBtn;
    QRadioButton* mStandardBtn;
    QRadioButton* mHighBtn;
    QLabel* mStrategySummary;        // 策略解释 (多行)
    QComboBox* mCoarseEngineCombo;   // 粗配准引擎覆盖: 策略默认/FFT幅度/NCC
    QSpinBox* mCoarseWindow;
    QSpinBox* mSearchWindow;
    QSpinBox* mFineWindow;
    QSpinBox* mOffsetPerBurst;
    QDoubleSpinBox* mCorrThreshold;
    QComboBox* mPolyDegree;

    // Tab 3: 重采样
    QComboBox* mResamplingMethod;
    QSpinBox* mSincWindow;
    QDoubleSpinBox* mSincBeta;
    QLineEdit* mDemPath;            // 地形校正 DEM (可选)

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
    QCheckBox* mEstimateBaseline;
};

#endif // REGISTRATIONDIALOG_H
