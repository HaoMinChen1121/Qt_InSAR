#ifndef GEOCODINGDIALOG_H
#define GEOCODINGDIALOG_H

#include <QDialog>
#include "domain/params/GeocodingParams.h"

class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QCheckBox;

class GeocodingDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GeocodingDialog(QWidget* parent = nullptr);
    void setParams(const GeocodingParams& p);
    GeocodingParams params() const;

private:
    // Tab 1: 编码方法
    QComboBox* mMethod;
    QSpinBox* mPolyOrder;
    QLineEdit* mDemPath;
    QCheckBox* mTerrainCorr;

    // Tab 2: 坐标系统
    QSpinBox* mEpsg;
    QDoubleSpinBox* mResolution;
    QDoubleSpinBox* mWest, *mEast, *mSouth, *mNorth;

    // Tab 3: 输出
    QComboBox* mFormat;
    QLineEdit* mOutputDir;
    QLineEdit* mOutputPrefix;
};

#endif // GEOCODINGDIALOG_H
