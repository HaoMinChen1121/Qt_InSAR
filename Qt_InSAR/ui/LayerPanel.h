#ifndef LAYERPANEL_H
#define LAYERPANEL_H

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QToolBar;

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

public slots:
    void onLayerLoaded(const QString& id, const QString& name, const QString& type);
    void onLayerRemoved(const QString& id);
    void onLayerError(const QString& errorMsg);
    void onAddLayer();

private slots:
    void onRemoveLayer();
    void onMoveUp();
    void onMoveDown();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onItemSelectionChanged();
    void onContextMenu(const QPoint& pos);

private:
    void setupUI();
    QTreeWidget* mTree;
    QToolBar* mToolbar;
};

#endif // LAYERPANEL_H
