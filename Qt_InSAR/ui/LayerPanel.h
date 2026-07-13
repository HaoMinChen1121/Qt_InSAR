#ifndef LAYERPANEL_H
#define LAYERPANEL_H

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QToolBar;
class QSlider;
class QLabel;

class LayerPanel : public QWidget
{
    Q_OBJECT
public:
    explicit LayerPanel(QWidget* parent = nullptr);

signals:
    void layerAddRequested(const QStringList& files);
    void layerRemoveRequested(const QStringList& ids);
    void layerVisibilityChanged(const QString& id, bool visible);
    void layerOrderChanged(const QStringList& orderedIds);
    void zoomToLayerRequested(const QString& id);
    void layerSelectionChanged(const QString& id);
    void opacityChanged(const QString& id, double opacity);
    void fullExtentRequested();
    void colorRampRequested(const QString& layerId);

public slots:
    void onLayerLoaded(const QString& id, const QString& name, const QString& type,
                       const QString& groupName = QString());
    void onLayerRemoved(const QString& id);
    void onLayerError(const QString& errorMsg);
    void onAddLayer();

private:
    QTreeWidgetItem* ensureGroup(const QString& groupName);

private slots:
    void onRemoveLayer();
    void onMoveUp();
    void onMoveDown();
    void onZoomToLayer();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemSelectionChanged();
    void onContextMenu(const QPoint& pos);

private:
    void setupUI();
    QTreeWidget* mTree;
    QToolBar*    mToolbar;
    QSlider*     mOpacitySlider = nullptr;
    QLabel*      mOpacityLabel  = nullptr;
};

#endif // LAYERPANEL_H
