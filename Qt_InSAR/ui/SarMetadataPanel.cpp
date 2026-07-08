#include "SarMetadataPanel.h"

#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QFrame>

SarMetadataPanel::SarMetadataPanel(QWidget* parent) : QWidget(parent)
{
    setupUI();
}

void SarMetadataPanel::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    QScrollArea* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    QWidget* content = new QWidget(scroll);
    QFormLayout* form = new QFormLayout(content);
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFormAlignment(Qt::AlignTop);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);

    auto addRow = [&](const QString& label, QLabel*& valueLabel) {
        valueLabel = new QLabel(QStringLiteral("-"), content);
        valueLabel->setTextFormat(Qt::PlainText);
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(label, valueLabel);
    };

    addRow(QStringLiteral("传感器:"), mSensorType);
    addRow(QStringLiteral("采集时间:"), mAcquisitionTime);
    addRow(QStringLiteral("产品类型:"), mProductType);
    addRow(QStringLiteral("极化方式:"), mPolarization);
    addRow(QStringLiteral("波长:"), mWavelength);
    addRow(QStringLiteral("距离向采样:"), mRangeSpacing);
    addRow(QStringLiteral("方位向采样:"), mAzimuthSpacing);
    addRow(QStringLiteral("近距:"), mNearRange);
    addRow(QStringLiteral("远距:"), mFarRange);
    addRow(QStringLiteral("PRF:"), mPrf);
    addRow(QStringLiteral("中心频率:"), mCenterFreq);
    addRow(QStringLiteral("轨道方向:"), mOrbitDirection);
    addRow(QStringLiteral("轨道号:"), mOrbitNumber);
    addRow(QStringLiteral("处理级别:"), mProcessingLevel);

    scroll->setWidget(content);
    mainLayout->addWidget(scroll);
}

void SarMetadataPanel::clearMetadata()
{
    QLabel* fields[] = {
        mSensorType, mAcquisitionTime, mProductType, mPolarization,
        mWavelength, mRangeSpacing, mAzimuthSpacing, mNearRange,
        mFarRange, mPrf, mCenterFreq, mOrbitDirection, mOrbitNumber,
        mProcessingLevel
    };
    for (auto* f : fields)
        f->setText(QStringLiteral("-"));
}

void SarMetadataPanel::setMetadata(
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
    const QString& processingLevel)
{
    mSensorType->setText(sensorType);
    mAcquisitionTime->setText(acquisitionTime);
    mProductType->setText(productType);
    mPolarization->setText(polarization);
    mWavelength->setText(QStringLiteral("%1 m").arg(wavelength, 0, 'f', 4));
    mRangeSpacing->setText(QStringLiteral("%1 m").arg(rangeSpacing, 0, 'f', 2));
    mAzimuthSpacing->setText(QStringLiteral("%1 m").arg(azimuthSpacing, 0, 'f', 2));
    mNearRange->setText(QStringLiteral("%1 m").arg(nearRange, 0, 'f', 0));
    mFarRange->setText(QStringLiteral("%1 m").arg(farRange, 0, 'f', 0));
    mPrf->setText(QStringLiteral("%1 Hz").arg(prf, 0, 'f', 2));
    mCenterFreq->setText(QStringLiteral("%1 Hz").arg(centerFreq, 0, 'f', 0));
    mOrbitDirection->setText(orbitDirection);
    mOrbitNumber->setText(orbitNumber > 0 ? QString::number(orbitNumber) : QStringLiteral("-"));
    mProcessingLevel->setText(processingLevel);
}
