#include "MapCanvasWidget.h"

#include <qgsmapcanvas.h>
#include <qgsmaptoolpan.h>
#include <qgsmaptoolzoom.h>
#include <qgsrectangle.h>
#include <qgspointxy.h>
#include <qgscoordinatereferencesystem.h>

#include <QVBoxLayout>

MapCanvasWidget::MapCanvasWidget(QWidget* parent) : QWidget(parent)
{
    setupUI();
    setupMapTools();
}

void MapCanvasWidget::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mCanvas = new QgsMapCanvas(this);
    mCanvas->setCanvasColor(QColor(255, 255, 255));
    mCanvas->enableAntiAliasing(true);

    layout->addWidget(mCanvas);
    setLayout(layout);

    connect(mCanvas, &QgsMapCanvas::extentsChanged,
            this, &MapCanvasWidget::onExtentChanged);
}

void MapCanvasWidget::setupMapTools()
{
    mPanTool = new QgsMapToolPan(mCanvas);
    mZoomTool = new QgsMapToolZoom(mCanvas, false);
    mCanvas->setMapTool(mPanTool);
}

QgsMapCanvas* MapCanvasWidget::mapCanvas() const { return mCanvas; }

// ===== private slots =====

void MapCanvasWidget::onExtentChanged()
{
    emit canvasExtentChanged(mCanvas->extent());
}

// ===== public slots (called by controller/service layer) =====

void MapCanvasWidget::setCanvasExtent(const QgsRectangle& extent)
{
    mCanvas->setExtent(extent);
    mCanvas->refresh();
}

void MapCanvasWidget::setCanvasCrs(const QgsCoordinateReferenceSystem& crs)
{
    mCanvas->setDestinationCrs(crs);
    mCanvas->refresh();
}

void MapCanvasWidget::setCanvasColor(const QColor& color)
{
    mCanvas->setCanvasColor(color);
    mCanvas->refresh();
}

void MapCanvasWidget::refreshCanvas()
{
    mCanvas->refresh();
}
