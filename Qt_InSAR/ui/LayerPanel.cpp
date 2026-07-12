#include "LayerPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTreeWidget>
#include <QToolBar>
#include <QAction>
#include <QSlider>
#include <QLabel>
#include <QFileDialog>
#include <QMenu>
#include <QHeaderView>
#include <QFont>
#include <QColor>

LayerPanel::LayerPanel(QWidget* parent) : QWidget(parent) { setupUI(); }

void LayerPanel::setupUI()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── 工具栏 ──
    mToolbar = new QToolBar(this);
    mToolbar->setIconSize(QSize(16, 16));
    mToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    mToolbar->addAction(QIcon(":/icon/icon/save.svg"),
                        QStringLiteral("添加图层"), this, &LayerPanel::onAddLayer);
    mToolbar->addAction(QIcon(":/icon/icon/delete.svg"),
                        QStringLiteral("移除图层"), this, &LayerPanel::onRemoveLayer);
    mToolbar->addSeparator();
    mToolbar->addAction(QIcon(":/icon/icon/undo.svg"),
                        QStringLiteral("上移"), this, &LayerPanel::onMoveUp);
    mToolbar->addAction(QIcon(":/icon/icon/redo.svg"),
                        QStringLiteral("下移"), this, &LayerPanel::onMoveDown);
    mToolbar->addSeparator();
    mToolbar->addAction(QIcon(":/icon/icon/layout.svg"),
                        QStringLiteral("缩放到图层"), this, &LayerPanel::onZoomToLayer);
    layout->addWidget(mToolbar);

    // ── 图层树 ──
    mTree = new QTreeWidget(this);
    mTree->setHeaderLabels({QStringLiteral(""), QStringLiteral("图层名称"), QStringLiteral("类型")});
    mTree->setColumnCount(3);
    mTree->header()->setStretchLastSection(false);
    mTree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    mTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    mTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    mTree->setColumnWidth(0, 36);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setDragDropMode(QAbstractItemView::InternalMove);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    mTree->setAlternatingRowColors(true);
    mTree->setRootIsDecorated(true);
    mTree->setIndentation(18);
    layout->addWidget(mTree);

    // ── 透明度控制 ──
    auto* opacityLayout = new QHBoxLayout();
    auto* opacityTitle = new QLabel(QStringLiteral("透明度:"), this);
    mOpacityLabel = new QLabel("100%", this);
    mOpacityLabel->setFixedWidth(40);
    mOpacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mOpacitySlider = new QSlider(Qt::Horizontal, this);
    mOpacitySlider->setRange(0, 100);
    mOpacitySlider->setValue(100);
    mOpacitySlider->setTickPosition(QSlider::NoTicks);
    opacityLayout->addWidget(opacityTitle);
    opacityLayout->addWidget(mOpacitySlider);
    opacityLayout->addWidget(mOpacityLabel);
    layout->addLayout(opacityLayout);

    connect(mOpacitySlider, &QSlider::valueChanged, this, [this](int val) {
        mOpacityLabel->setText(QStringLiteral("%1%").arg(val));
        QList<QTreeWidgetItem*> sel = mTree->selectedItems();
        if (!sel.isEmpty()) {
            QString id = sel.first()->data(0, Qt::UserRole).toString();
            if (!id.isEmpty() && id != "__group__")
                emit opacityChanged(id, val / 100.0);
        }
    });

    connect(mTree, &QTreeWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(mTree, &QTreeWidget::itemSelectionChanged, this, &LayerPanel::onItemSelectionChanged);
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, &LayerPanel::onContextMenu);
}

// ── 添加图层 ──
void LayerPanel::onAddLayer()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择图层文件"), QString(),
        QStringLiteral("所有支持格式 (*.zip *.SAFE *.tif *.tiff *.img);;"
                       "Sentinel-1 (*.zip *.SAFE);;"
                       "通用栅格 (*.tif *.tiff *.img);;"
                       "所有文件 (*.*)"));
    if (!files.isEmpty())
        emit layerAddRequested(files);
}

// ── 移除图层（多选支持） ──
void LayerPanel::onRemoveLayer()
{
    QList<QTreeWidgetItem*> selected = mTree->selectedItems();
    if (selected.isEmpty()) return;

    QStringList ids;
    for (auto* item : selected) {
        QString id = item->data(0, Qt::UserRole).toString();
        if (id == "__group__") {
            // 移除整个产品分组
            for (int i = 0; i < item->childCount(); ++i)
                ids << item->child(i)->data(0, Qt::UserRole).toString();
        } else if (!id.isEmpty()) {
            ids << id;
        }
    }
    if (!ids.isEmpty())
        emit layerRemoveRequested(ids);
}

// ── 缩放到图层 ──
void LayerPanel::onZoomToLayer()
{
    QList<QTreeWidgetItem*> sel = mTree->selectedItems();
    if (sel.isEmpty()) return;
    QString id = sel.first()->data(0, Qt::UserRole).toString();
    if (!id.isEmpty() && id != "__group__")
        emit zoomToLayerRequested(id);
}

// ── 上移/下移 ──
void LayerPanel::onMoveUp()
{
    QList<QTreeWidgetItem*> sel = mTree->selectedItems();
    if (sel.isEmpty()) return;
    QTreeWidgetItem* item = sel.first();
    QTreeWidgetItem* parent = item->parent();
    int idx = parent ? parent->indexOfChild(item) : mTree->indexOfTopLevelItem(item);
    if (idx <= 0) return;
    QTreeWidgetItem* taken = parent ? parent->takeChild(idx) : mTree->takeTopLevelItem(idx);
    if (parent) parent->insertChild(idx - 1, taken);
    else mTree->insertTopLevelItem(idx - 1, taken);
    mTree->setCurrentItem(taken);
}

void LayerPanel::onMoveDown()
{
    QList<QTreeWidgetItem*> sel = mTree->selectedItems();
    if (sel.isEmpty()) return;
    QTreeWidgetItem* item = sel.first();
    QTreeWidgetItem* parent = item->parent();
    int count = parent ? parent->childCount() : mTree->topLevelItemCount();
    int idx = parent ? parent->indexOfChild(item) : mTree->indexOfTopLevelItem(item);
    if (idx < 0 || idx >= count - 1) return;
    QTreeWidgetItem* taken = parent ? parent->takeChild(idx) : mTree->takeTopLevelItem(idx);
    if (parent) parent->insertChild(idx + 1, taken);
    else mTree->insertTopLevelItem(idx + 1, taken);
    mTree->setCurrentItem(taken);
}

// ── 复选框变化 ──
void LayerPanel::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (column != 0) return;
    QString id = item->data(0, Qt::UserRole).toString();
    if (id == "__group__") return;
    if (id.isEmpty()) return;
    bool visible = (item->checkState(0) == Qt::Checked);
    emit layerVisibilityChanged(id, visible);
}

// ── 选中变化 ──
void LayerPanel::onItemSelectionChanged()
{
    QList<QTreeWidgetItem*> sel = mTree->selectedItems();
    if (!sel.isEmpty()) {
        QString id = sel.first()->data(0, Qt::UserRole).toString();
        if (!id.isEmpty() && id != "__group__")
            emit layerSelectionChanged(id);
    }
}

// ── 右键菜单 ──
void LayerPanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = mTree->itemAt(pos);
    if (!item) return;
    QString id = item->data(0, Qt::UserRole).toString();

    QMenu menu(this);
    QAction* actZoom = menu.addAction(QStringLiteral("缩放到图层"));
    menu.addSeparator();
    QAction* actRemove = nullptr;
    QAction* actUp     = menu.addAction(QStringLiteral("上移"));
    QAction* actDown   = menu.addAction(QStringLiteral("下移"));
    menu.addSeparator();
    actRemove = menu.addAction(QStringLiteral("移除"));

    if (id == "__group__") {
        actRemove->setText(QStringLiteral("移除整个产品"));
    }

    QAction* selected = menu.exec(mTree->viewport()->mapToGlobal(pos));
    if (!selected) return;

    if (selected == actZoom)
        onZoomToLayer();
    else if (selected == actUp)
        onMoveUp();
    else if (selected == actDown)
        onMoveDown();
    else if (selected == actRemove)
        onRemoveLayer();
}

// ── 分组管理 ──
QTreeWidgetItem* LayerPanel::ensureGroup(const QString& groupName)
{
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = mTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == "__group__"
            && item->text(1) == groupName)
            return item;
    }

    auto* group = new QTreeWidgetItem();
    group->setCheckState(0, Qt::Checked);
    group->setText(1, groupName);
    group->setText(2, QStringLiteral("产品"));
    group->setData(0, Qt::UserRole, "__group__");
    group->setFlags(group->flags() | Qt::ItemIsUserCheckable);
    QFont f = group->font(1);
    f.setBold(true);
    group->setFont(1, f);
    group->setBackground(1, QColor("#E8ECF0"));
    group->setBackground(2, QColor("#E8ECF0"));
    group->setExpanded(true);
    mTree->insertTopLevelItem(0, group);
    return group;
}

// ── 图层加载 ──
void LayerPanel::onLayerLoaded(const QString& id, const QString& name,
                               const QString& type, const QString& groupName)
{
    // 检查重复
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = mTree->topLevelItem(i);
        if (top->data(0, Qt::UserRole).toString() == id) return;
        for (int j = 0; j < top->childCount(); ++j) {
            if (top->child(j)->data(0, Qt::UserRole).toString() == id) return;
        }
    }

    auto* item = new QTreeWidgetItem();
    item->setCheckState(0, Qt::Checked);
    item->setText(1, name);
    item->setText(2, type);
    item->setData(0, Qt::UserRole, id);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);

    if (groupName.isEmpty()) {
        mTree->insertTopLevelItem(0, item);
    } else {
        QTreeWidgetItem* group = ensureGroup(groupName);
        group->insertChild(0, item);

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

// ── 图层移除 ──
void LayerPanel::onLayerRemoved(const QString& id)
{
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = mTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == id) {
            delete mTree->takeTopLevelItem(i);
            return;
        }
        for (int j = 0; j < item->childCount(); ++j) {
            QTreeWidgetItem* child = item->child(j);
            if (child->data(0, Qt::UserRole).toString() == id) {
                delete item->takeChild(j);
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
