#ifndef OVERVIEWWIDGET_H
#define OVERVIEWWIDGET_H

#include <QWidget>
#include <qgsrectangle.h>

class QgsMapCanvas;
class QToolBar;
class QAction;

/**
 * @brief 鹰眼视图控件（表示层）
 *
 * 绑定主地图画布，在缩略图上显示当前视口矩形框。
 * - 拖拽矩形框: 平移主画布
 * - 滚轮: 缩放主画布
 */
class OverviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit OverviewWidget(QWidget* parent = nullptr);

    void setMainCanvas(QgsMapCanvas* canvas);

public slots:
    void onMainExtentChanged();
    void refreshOverview();

signals:
    void extentChangeRequested(const QgsRectangle& newExtent);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateViewportRect();
    QgsRectangle widgetPointToExtent(const QPointF& pos) const;
    QRectF extentToWidgetRect() const;

    QgsMapCanvas* mMainCanvas = nullptr;
    QImage mThumbnail;
    QgsRectangle mFullExtent;
    QRectF mViewportRect;       // 归一化坐标 [0,1]
    QPointF mDragStart;
    bool mDragging = false;
    bool mSyncEnabled = true;
};

#endif // OVERVIEWWIDGET_H
