#ifndef SLCMETADATADIALOG_H
#define SLCMETADATADIALOG_H

#include <QDialog>
#include <QComboBox>
#include "dataaccess/annotation/SlcAnnotation.h"
#include "controllers/ApplicationController.h"

class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QLabel;

class SlcMetadataDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SlcMetadataDialog(const QMap<QString, ProductSourceInfo>& allProducts,
                               const QString& initialKey,
                               QWidget* parent = nullptr);

private slots:
    void onProductChanged(int index);
    void onBandChanged(int index);

private:
    void updateInfoBar();
    void populateTree();
    void repopulate();
    void rebuildBandCombo();

    // 辅助
    QTreeWidgetItem* addNode(QTreeWidgetItem* parent,
        const QString& label, const QString& value,
        const QColor& color = Qt::black);
    QTreeWidgetItem* addBoolNode(QTreeWidgetItem* parent,
        const QString& label, bool value);
    QTreeWidgetItem* addHeader(QTreeWidgetItem* parent, const QString& title);

    // 9 个模块填充
    void fillAdsHeader(QTreeWidgetItem* root);
    void fillQuality(QTreeWidgetItem* root);
    void fillGeneralAnnotation(QTreeWidgetItem* root);
    void fillImageAnnotation(QTreeWidgetItem* root);
    void fillDoppler(QTreeWidgetItem* root);
    void fillSwathTiming(QTreeWidgetItem* root);
    void fillGeolocationGrid(QTreeWidgetItem* root);

    // 按钮
    void copySelectedValue();
    void copyAll();
    void exportJson();

    // 颜色常量
    static const QColor kColorRed;
    static const QColor kColorOrange;
    static const QColor kColorGray;

    QMap<QString, ProductSourceInfo> mAllProducts;
    QString              mCurrentKey;
    QString              mCurrentBand;  // "IW1/VH"
    SlcAnnotation        mAnn;

    QComboBox*           mProductCombo;
    QComboBox*           mBandCombo;
    QLabel*              mInfoLabel;
    QTreeWidget*         mTree;
    QPlainTextEdit*      mDetailView;
};

#endif // SLCMETADATADIALOG_H
