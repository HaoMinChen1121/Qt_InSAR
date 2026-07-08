#include "LayerPanel.h"
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QToolBar>
#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QHeaderView>

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent) { setupUI(); }

void LayerPanel::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mToolbar = new QToolBar(this);
    mToolbar->addAction(QStringLiteral("+"), this, &LayerPanel::onAddLayer);
    mToolbar->addAction(QStringLiteral("-"), this, &LayerPanel::onRemoveLayer);
    mToolbar->addAction(QStringLiteral("↑"), this, &LayerPanel::onMoveUp);
    mToolbar->addAction(QStringLiteral("↓"), this, &LayerPanel::onMoveDown);
    layout->addWidget(mToolbar);

    mTree = new QTreeWidget(this);
    mTree->setHeaderLabels({QStringLiteral("图层"), QStringLiteral("类型")});
    mTree->header()->setStretchLastSection(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setDragDropMode(QAbstractItemView::InternalMove);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(mTree);

    connect(mTree, &QTreeWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(mTree, &QTreeWidget::itemSelectionChanged, this, &LayerPanel::onItemSelectionChanged);
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, &LayerPanel::onContextMenu);
}

void LayerPanel::onAddLayer()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择影像文件"), QString(),
        QStringLiteral("栅格文件 (*.tif *.tiff *.img *.dat *.raw *.slc);;所有文件 (*.*)"));
    if (!files.isEmpty())
        emit layerAddRequested(files);
}

void LayerPanel::onRemoveLayer()
{
    QList<QTreeWidgetItem*> selected = mTree->selectedItems();
    QStringList ids;
    for (auto* item : selected)
        ids << item->data(0, Qt::UserRole).toString();
    if (!ids.isEmpty())
        emit layerRemoveRequested(ids);
}

void LayerPanel::onMoveUp() { /* TODO */ }
void LayerPanel::onMoveDown() { /* TODO */ }

void LayerPanel::onItemChanged(QTreeWidgetItem* item, int /*column*/)
{
    QString id = item->data(0, Qt::UserRole).toString();
    bool visible = (item->checkState(0) == Qt::Checked);
    emit layerVisibilityChanged(id, visible);
}

void LayerPanel::onItemSelectionChanged()
{
    QList<QTreeWidgetItem*> selected = mTree->selectedItems();
    if (!selected.isEmpty())
        emit layerSelectionChanged(selected.first()->data(0, Qt::UserRole).toString());
}

void LayerPanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = mTree->itemAt(pos);
    if (!item) return;
    QMenu menu(this);
    QAction* zoomAct = menu.addAction(QStringLiteral("缩放至图层"));
    menu.addSeparator();
    QAction* removeAct = menu.addAction(QStringLiteral("移除"));
    QAction* selected = menu.exec(mTree->viewport()->mapToGlobal(pos));
    if (selected == zoomAct)
        emit zoomToLayerRequested(item->data(0, Qt::UserRole).toString());
    else if (selected == removeAct)
        emit layerRemoveRequested({item->data(0, Qt::UserRole).toString()});
}

void LayerPanel::onLayerLoaded(const QString& id, const QString& name, const QString& type)
{
    auto* item = new QTreeWidgetItem();
    item->setText(0, name);
    item->setText(1, type);
    item->setData(0, Qt::UserRole, id);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, Qt::Checked);
    mTree->insertTopLevelItem(0, item);
}

void LayerPanel::onLayerRemoved(const QString& id)
{
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = mTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == id) {
            delete mTree->takeTopLevelItem(i);
            return;
        }
    }
}

void LayerPanel::onLayerError(const QString& errorMsg)
{
    Q_UNUSED(errorMsg);
}
