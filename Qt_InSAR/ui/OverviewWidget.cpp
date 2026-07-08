#include "OverviewWidget.h"

#include <qgsmapcanvas.h>
#include <qgsmaprenderersequentialjob.h>
#include <qgsmapsettings.h>
#include <qgsrectangle.h>

#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QVBoxLayout>

OverviewWidget::OverviewWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(180, 130);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setMouseTracking(true);
}

void OverviewWidget::setMainCanvas(QgsMapCanvas* canvas)
{
    if (mMainCanvas)
        disconnect(mMainCanvas, nullptr, this, nullptr);

    mMainCanvas = canvas;

    if (mMainCanvas) {
        connect(mMainCanvas, &QgsMapCanvas::extentsChanged,
                this, &OverviewWidget::onMainExtentChanged);
        connect(mMainCanvas, &QgsMapCanvas::renderComplete,
                this, &OverviewWidget::refreshOverview);
    }
}

void OverviewWidget::onMainExtentChanged()
{
    updateViewportRect();
    update();
}

void OverviewWidget::refreshOverview()
{
    if (!mMainCanvas)
        return;

    const auto& layers = mMainCanvas->mapSettings().layers();
    if (layers.isEmpty()) {
        mThumbnail = QImage();
        mFullExtent = QgsRectangle();
        update();
        return;
    }

    QgsRectangle fullExtent = mMainCanvas->fullExtent();
    if (fullExtent.isEmpty())
        return;

    QgsMapSettings settings;
    settings.setLayers(layers);
    settings.setExtent(fullExtent);
    settings.setDestinationCrs(mMainCanvas->mapSettings().destinationCrs());
    settings.setBackgroundColor(QColor(255, 255, 255));

    int w = width();
    int h = height();
    if (w < 1 || h < 1) return;

    double aspect = fullExtent.width() / fullExtent.height();
    if (aspect > static_cast<double>(w) / h)
        h = static_cast<int>(w / aspect);
    else
        w = static_cast<int>(h * aspect);
    settings.setOutputSize(QSize(qMax(w, 1), qMax(h, 1)));

    QgsMapRendererSequentialJob job(settings);
    job.start();
    job.waitForFinished();

    mThumbnail = job.renderedImage();
    mFullExtent = fullExtent;
    updateViewportRect();
    update();
}

void OverviewWidget::updateViewportRect()
{
    if (!mMainCanvas || mFullExtent.isEmpty()) {
        mViewportRect = QRectF(0, 0, 1, 1);
        return;
    }

    QgsRectangle canvasExtent = mMainCanvas->extent();
    double fx = (canvasExtent.xMinimum() - mFullExtent.xMinimum()) / mFullExtent.width();
    double fy = (mFullExtent.yMaximum() - canvasExtent.yMaximum()) / mFullExtent.height();
    double fw = canvasExtent.width() / mFullExtent.width();
    double fh = canvasExtent.height() / mFullExtent.height();
    mViewportRect = QRectF(fx, fy, fw, fh);
}

QRectF OverviewWidget::extentToWidgetRect() const
{
    if (mThumbnail.isNull() || mFullExtent.isEmpty())
        return QRectF(0, 0, width(), height());

    double scaleX = static_cast<double>(width()) / mFullExtent.width();
    double scaleY = static_cast<double>(height()) / mFullExtent.height();
    double scale = qMin(scaleX, scaleY);

    double imgW = mFullExtent.width() * scale;
    double imgH = mFullExtent.height() * scale;
    double ox = (width() - imgW) / 2.0;
    double oy = (height() - imgH) / 2.0;

    return QRectF(ox + mViewportRect.x() * imgW,
                  oy + mViewportRect.y() * imgH,
                  mViewportRect.width() * imgW,
                  mViewportRect.height() * imgH);
}

QgsRectangle OverviewWidget::widgetPointToExtent(const QPointF& pos) const
{
    if (mFullExtent.isEmpty())
        return QgsRectangle();

    double scaleX = static_cast<double>(width()) / mFullExtent.width();
    double scaleY = static_cast<double>(height()) / mFullExtent.height();
    double scale = qMin(scaleX, scaleY);

    double imgW = mFullExtent.width() * scale;
    double imgH = mFullExtent.height() * scale;
    double ox = (width() - imgW) / 2.0;
    double oy = (height() - imgH) / 2.0;

    double geoX = mFullExtent.xMinimum() + (pos.x() - ox) / scale;
    double geoY = mFullExtent.yMaximum() - (pos.y() - oy) / scale;
    return QgsRectangle(geoX, geoY, geoX, geoY);
}

void OverviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 背景
    painter.fillRect(rect(), QColor(220, 220, 220));

    if (mThumbnail.isNull()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("无图层"));
        return;
    }

    // 计算缩略图绘制区域（居中保持比例）
    double scaleX = static_cast<double>(width()) / mFullExtent.width();
    double scaleY = static_cast<double>(height()) / mFullExtent.height();
    double scale = qMin(scaleX, scaleY);
    double imgW = mFullExtent.width() * scale;
    double imgH = mFullExtent.height() * scale;
    double ox = (width() - imgW) / 2.0;
    double oy = (height() - imgH) / 2.0;

    // 绘制缩略图
    painter.drawImage(QRectF(ox, oy, imgW, imgH), mThumbnail);

    // 绘制视口矩形框（红色）
    QRectF vr = extentToWidgetRect();
    QPen pen(QColor(230, 50, 50), 2);
    pen.setStyle(Qt::SolidLine);
    painter.setPen(pen);
    painter.setBrush(QColor(230, 50, 50, 30));
    painter.drawRect(vr);

    // 外边框
    painter.setPen(QPen(QColor(100, 100, 100), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(ox, oy, imgW, imgH));
}

void OverviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (mThumbnail.isNull()) return;

    QRectF vr = extentToWidgetRect();
    if (vr.contains(event->pos())) {
        mDragging = true;
        mDragStart = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void OverviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (mDragging) {
        QgsRectangle curExtent = mMainCanvas ? mMainCanvas->extent()
                                             : QgsRectangle();
        if (curExtent.isEmpty()) return;

        QPointF delta = event->pos() - mDragStart;
        // 像素偏移 → 地理偏移
        double scaleX = static_cast<double>(width()) / mFullExtent.width();
        double scaleY = static_cast<double>(height()) / mFullExtent.height();
        double scale = qMin(scaleX, scaleY);
        double geoDx = delta.x() / scale;    // 拖动方向 = 想看的方向
        double geoDy = -delta.y() / scale;   // screen y↓ → geo y↓ (南)

        QgsRectangle newExtent(
            curExtent.xMinimum() + geoDx,
            curExtent.yMinimum() + geoDy,
            curExtent.xMaximum() + geoDx,
            curExtent.yMaximum() + geoDy
        );

        // 限制不超出全图
        if (newExtent.xMinimum() < mFullExtent.xMinimum())
            newExtent.setXMinimum(mFullExtent.xMinimum());
        if (newExtent.yMinimum() < mFullExtent.yMinimum())
            newExtent.setYMinimum(mFullExtent.yMinimum());
        if (newExtent.xMaximum() > mFullExtent.xMaximum())
            newExtent.setXMaximum(mFullExtent.xMaximum());
        if (newExtent.yMaximum() > mFullExtent.yMaximum())
            newExtent.setYMaximum(mFullExtent.yMaximum());

        mDragStart = event->pos();
        emit extentChangeRequested(newExtent);
    } else if (!mThumbnail.isNull()) {
        QRectF vr = extentToWidgetRect();
        setCursor(vr.contains(event->pos()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }
}

void OverviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        mDragging = false;
        setCursor(Qt::ArrowCursor);
    }
}

void OverviewWidget::wheelEvent(QWheelEvent* event)
{
    if (!mMainCanvas || mFullExtent.isEmpty()) return;

    QgsRectangle curExtent = mMainCanvas->extent();
    double factor = (event->angleDelta().y() > 0) ? 0.8 : 1.25;

    double cx = curExtent.xMinimum() + curExtent.width() / 2.0;
    double cy = curExtent.yMinimum() + curExtent.height() / 2.0;
    double newW = curExtent.width() * factor;
    double newH = curExtent.height() * factor;

    // 限制在 fullExtent 范围内
    if (newW > mFullExtent.width())  newW = mFullExtent.width();
    if (newH > mFullExtent.height()) newH = mFullExtent.height();

    QgsRectangle newExtent(
        cx - newW / 2.0, cy - newH / 2.0,
        cx + newW / 2.0, cy + newH / 2.0);

    emit extentChangeRequested(newExtent);
}

void OverviewWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (mMainCanvas && !mMainCanvas->mapSettings().layers().isEmpty())
        refreshOverview();
}
