#ifndef REGISTRATIONDIALOG_H
#define REGISTRATIONDIALOG_H

#include <QDialog>
#include <QButtonGroup>
#include <QStackedWidget>
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
    void onRouteChanged(int routeIdx);

private:
    QWidget* createRoute1Page();
    QWidget* createRoute2Page();
    QWidget* createRoute3Page();

    // 保留元数据 (路径/轨道/传感器)，不被对话框覆盖
    RegistrationParams mMetaHolder;

    // Tab 1: 主辅影像
    QLineEdit* mMasterPath;
    QLineEdit* mSlavePath;
    QLabel* mMasterMeta;
    QLabel* mSlaveMeta;

    // 路线选择
    QButtonGroup* mRouteGroup;
    QRadioButton* mRoute1Btn;
    QRadioButton* mRoute2Btn;
    QRadioButton* mRoute3Btn;
    QStackedWidget* mRouteStack;

    // ── Route1 控件 ──
    QSpinBox* mR1CoarseFFT;
    QSpinBox* mR1ControlPoints;

    // ── Route2 控件 ──
    QSpinBox* mR2NccWindow;
    QSpinBox* mR2SearchWindow;
    QSpinBox* mR2ControlPoints;
    QSpinBox* mR2FineWindow;
    QDoubleSpinBox* mR2CorrThreshold;
    QComboBox* mR2PolyDegree;
    QCheckBox* mR2EnableFineFFT;
    QSpinBox* mR2FineFFTWindow;

    // ── Route3 控件 ──
    QSpinBox* mR3CoarseFFT;
    QSpinBox* mR3ControlPoints;
    QSpinBox* mR3FineWindow;
    QDoubleSpinBox* mR3CorrThreshold;
    QComboBox* mR3PolyDegree;
    QCheckBox* mR3EnableFineFFT;
    QSpinBox* mR3FineFFTWindow;

    // ESD (路线2/3)
    QCheckBox* mEnableEsd;

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
