#include "ColorRampDialog.h"

#include <qgsrasterlayer.h>
#include <qgsrastershader.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgscolorrampshader.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QPainter>
#include <QPixmap>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ColorRampDialog::ColorRampDialog(QgsRasterLayer* layer, QWidget* parent)
    : QDialog(parent), mLayer(layer)
{
    setWindowTitle(QStringLiteral("彩色渲染"));
    setMinimumWidth(380);

    mLayerName = layer ? layer->name() : QString();

    auto* mainLayout = new QVBoxLayout(this);

    QLabel* title = new QLabel(QStringLiteral("图层: %1").arg(mLayerName), this);
    title->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(title);

    auto* form = new QFormLayout;

    mPresetCombo = new QComboBox(this);
    mPresetCombo->addItem(QStringLiteral("相位 cyclic (-π~π)"), 0);
    mPresetCombo->addItem(QStringLiteral("相干性 (0~1)"), 1);
    mPresetCombo->addItem(QStringLiteral("灰度 (auto)"), 2);
    form->addRow(QStringLiteral("预设:"), mPresetCombo);

    mMinSpin = new QDoubleSpinBox(this);
    mMinSpin->setRange(-1e6, 1e6);
    mMinSpin->setDecimals(4);
    form->addRow(QStringLiteral("最小值:"), mMinSpin);

    mMaxSpin = new QDoubleSpinBox(this);
    mMaxSpin->setRange(-1e6, 1e6);
    mMaxSpin->setDecimals(4);
    form->addRow(QStringLiteral("最大值:"), mMaxSpin);

    mainLayout->addLayout(form);

    mPreviewLabel = new QLabel(this);
    mPreviewLabel->setFixedHeight(30);
    mainLayout->addWidget(mPreviewLabel);

    auto* btnBox = new QDialogButtonBox(this);
    QPushButton* applyBtn = btnBox->addButton(QStringLiteral("应用"), QDialogButtonBox::AcceptRole);
    btnBox->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(btnBox);

    connect(applyBtn, &QPushButton::clicked, this, &ColorRampDialog::onApply);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(mPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ColorRampDialog::onPresetChanged);

    // 默认相位
    onPresetChanged(0);
}

void ColorRampDialog::onPresetChanged(int index)
{
    switch (index) {
    case 0: // 相位 cyclic
        mMinSpin->setValue(-M_PI);
        mMaxSpin->setValue(M_PI);
        break;
    case 1: // 相干性
        mMinSpin->setValue(0.0);
        mMaxSpin->setValue(1.0);
        break;
    case 2: // 灰度
        mMinSpin->setValue(0.0);
        mMaxSpin->setValue(1.0);
        break;
    }
}

void ColorRampDialog::onApply()
{
    double minVal = mMinSpin->value();
    double maxVal = mMaxSpin->value();

    QList<QPair<double, QColor>> stops;
    int preset = mPresetCombo->currentData().toInt();

    if (preset == 0) {
        // 相位 cyclic: HSV 循环色带
        for (int i = 0; i <= 256; ++i) {
            double v = minVal + (maxVal - minVal) * i / 256.0;
            double h = (i % 256) / 256.0;
            stops.append({v, QColor::fromHsvF(h, 0.8, 0.9)});
        }
    } else if (preset == 1) {
        // 相干性: 黑→红→黄→白
        stops.append({minVal, QColor(0,0,0)});
        stops.append({minVal + 0.3*(maxVal-minVal), QColor(200,0,0)});
        stops.append({minVal + 0.6*(maxVal-minVal), QColor(255,255,0)});
        stops.append({maxVal, QColor(255,255,255)});
    } else {
        // 灰度
        stops.append({minVal, QColor(0,0,0)});
        stops.append({maxVal, QColor(255,255,255)});
    }

    applyRamp(minVal, maxVal, stops);
    accept();
}

void ColorRampDialog::applyRamp(double minVal, double maxVal,
                                const QList<QPair<double, QColor>>& stops)
{
    if (!mLayer) return;

    QList<QgsColorRampShader::ColorRampItem> items;
    for (const auto& s : stops)
        items.append(QgsColorRampShader::ColorRampItem(s.first, s.second, QString()));

    auto* shader = new QgsColorRampShader(minVal, maxVal);
    shader->setColorRampItemList(items);
    shader->setColorRampType(QgsColorRampShader::Interpolated);
    shader->classifyColorRamp();

    auto* renderer = new QgsSingleBandPseudoColorRenderer(
        mLayer->dataProvider(), 1, shader);
    mLayer->setRenderer(renderer);
    mLayer->triggerRepaint();
}
