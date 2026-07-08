#ifndef SARMETADATAPANEL_H
#define SARMETADATAPANEL_H

#include <QWidget>

class QLabel;
class QFormLayout;
class QScrollArea;

/**
 * @brief SAR 元数据信息面板（表示层，只读显示）
 *
 * 显示当前选中图层的传感器、轨道、采集参数等信息。
 * 由 ApplicationController 通过 setMetadata() 更新内容。
 */
class SarMetadataPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SarMetadataPanel(QWidget* parent = nullptr);

public slots:
    void clearMetadata();
    void setMetadata(
        const QString& sensorType,
        const QString& acquisitionTime,
        const QString& productType,
        const QString& polarization,
        double wavelength,
        double rangeSpacing,
        double azimuthSpacing,
        double nearRange,
        double farRange,
        double prf,
        double centerFreq,
        const QString& orbitDirection,
        int orbitNumber,
        const QString& processingLevel);

private:
    void setupUI();

    QLabel* mSensorType;
    QLabel* mAcquisitionTime;
    QLabel* mProductType;
    QLabel* mPolarization;
    QLabel* mWavelength;
    QLabel* mRangeSpacing;
    QLabel* mAzimuthSpacing;
    QLabel* mNearRange;
    QLabel* mFarRange;
    QLabel* mPrf;
    QLabel* mCenterFreq;
    QLabel* mOrbitDirection;
    QLabel* mOrbitNumber;
    QLabel* mProcessingLevel;
};

#endif // SARMETADATAPANEL_H
