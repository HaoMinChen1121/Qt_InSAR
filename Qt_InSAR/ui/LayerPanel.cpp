#include "LayerPanel.h"
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QToolBar>
#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QHeaderView>
#include <QFont>
#include <QColor>

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
    if (id.isEmpty()) return; // 分组标题项跳过
    bool visible = (item->checkState(0) == Qt::Checked);
    emit layerVisibilityChanged(id, visible);
}

void LayerPanel::onItemSelectionChanged()
{
    QList<QTreeWidgetItem*> selected = mTree->selectedItems();
    if (!selected.isEmpty()) {
        QString id = selected.first()->data(0, Qt::UserRole).toString();
        if (!id.isEmpty())
            emit layerSelectionChanged(id);
    }
}

void LayerPanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = mTree->itemAt(pos);
    if (!item) return;
    QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty()) return; // 分组标题

    QMenu menu(this);
    QAction* zoomAct = menu.addAction(QStringLiteral("缩放至图层"));
    menu.addSeparator();
    QAction* removeAct = menu.addAction(QStringLiteral("移除"));
    QAction* selected = menu.exec(mTree->viewport()->mapToGlobal(pos));
    if (selected == zoomAct)
        emit zoomToLayerRequested(id);
    else if (selected == removeAct)
        emit layerRemoveRequested({id});
}

QTreeWidgetItem* LayerPanel::ensureGroup(const QString& groupName)
{
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = mTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == "__group__"
            && item->text(0) == groupName)
            return item;
    }

    auto* group = new QTreeWidgetItem();
    group->setText(0, groupName);
    group->setText(1, QStringLiteral("产品"));
    group->setData(0, Qt::UserRole, QStringLiteral("__group__"));
    group->setFlags(group->flags() & ~Qt::ItemIsUserCheckable);
    QFont f = group->font(0);
    f.setBold(true);
    group->setFont(0, f);
    group->setBackground(0, QColor("#E8ECF0"));
    group->setBackground(1, QColor("#E8ECF0"));
    group->setExpanded(true);
    mTree->insertTopLevelItem(0, group);
    return group;
}

void LayerPanel::onLayerLoaded(const QString& id, const QString& name,
                               const QString& type, const QString& groupName)
{
    auto* item = new QTreeWidgetItem();
    item->setText(0, name);
    item->setText(1, type);
    item->setData(0, Qt::UserRole, id);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, Qt::Checked);

    if (groupName.isEmpty()) {
        mTree->insertTopLevelItem(0, item);
    } else {
        QTreeWidgetItem* group = ensureGroup(groupName);
        group->insertChild(0, item);
        // 分组可见性控制子图层
        connect(mTree, &QTreeWidget::itemChanged, this,
            [this, group](QTreeWidgetItem* changed, int col) {
                if (changed == group && col == 0) {
                    bool vis = (group->checkState(0) == Qt::Checked);
                    for (int i = 0; i < group->childCount(); ++i) {
                        QTreeWidgetItem* child = group->child(i);
                        child->setCheckState(0, vis ? Qt::Checked : Qt::Unchecked);
                        QString cid = child->data(0, Qt::UserRole).toString();
                        if (!cid.isEmpty())
                            emit layerVisibilityChanged(cid, vis);
                    }
                }
            }, Qt::UniqueConnection);
    }
}

void LayerPanel::onLayerRemoved(const QString& id)
{
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = mTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == id) {
            delete mTree->takeTopLevelItem(i);
            return;
        }
        // 搜索分组内子项
        for (int j = 0; j < item->childCount(); ++j) {
            QTreeWidgetItem* child = item->child(j);
            if (child->data(0, Qt::UserRole).toString() == id) {
                delete item->takeChild(j);
                // 如果分组空了, 删除分组
                if (item->childCount() == 0)
                    delete mTree->takeTopLevelItem(i);
                return;
            }
        }
    }
}

void LayerPanel::onLayerError(const QString& errorMsg)
{
    Q_UNUSED(errorMsg);
}
