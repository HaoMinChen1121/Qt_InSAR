#ifndef COLORRAMPDIALOG_H
#define COLORRAMPDIALOG_H

#include <QDialog>
#include <QString>

class QgsRasterLayer;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

class ColorRampDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ColorRampDialog(QgsRasterLayer* layer, QWidget* parent = nullptr);

private slots:
    void onApply();
    void onPresetChanged(int index);

private:
    void applyRamp(double minVal, double maxVal, const QList<QPair<double, QColor>>& stops);

    QgsRasterLayer* mLayer;
    QComboBox*      mPresetCombo;
    QDoubleSpinBox* mMinSpin;
    QDoubleSpinBox* mMaxSpin;
    QLabel*         mPreviewLabel;
    QString         mLayerName;
};

#endif // COLORRAMPDIALOG_H
