#ifndef INTERFEROGRAMDIALOG_H
#define INTERFEROGRAMDIALOG_H

#include <QDialog>
#include "domain/params/InterferogramParams.h"

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

private:
    // Tab 0: 输入
    QLineEdit* mMasterQsar;
    QLineEdit* mSlaveQsar;

    // Tab 1: 干涉图
    QSpinBox* mRangeLooks;
    QSpinBox* mAzimuthLooks;
    QCheckBox* mAzRampCorr;         // deburst 方位时间校正
    QCheckBox* mPhaseAlign;         // IW 拼接相位一致性对齐
    QCheckBox* mVisColor;           // 生成 HSV 彩色渲染

    // Tab 2: 去平地
    QCheckBox* mEnableFlat;         // 启用去平地 (合并产品逐列几何)
    QLabel*    mIncAngleLabel;
    double     mCachedIncAngle     = 0.0;
    double     mCachedWavelength   = 0.0;
    double     mCachedNearRange    = 0.0;
    double     mCachedRangeSpacing = 0.0;
    double     mCachedPrf          = 0.0;

    // Tab 3: 差分
    QLineEdit* mDiffDemPath;
    QCheckBox* mEnableDiff;         // 启用差分 (DEM 地形相位去除)

    // Tab 4: 输出
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
    QCheckBox* mLegacyPerIw;        // 保留逐子条带中间输出 (兼容旧流程)
    QCheckBox* mAutoLoad;           // 完成后自动加载可见图层
};

#endif // INTERFEROGRAMDIALOG_H
