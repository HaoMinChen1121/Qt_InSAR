#ifndef MAPCANVASWIDGET_H
#define MAPCANVASWIDGET_H

#include <QWidget>

class QgsMapCanvas;
class QgsMapToolPan;
class QgsMapToolZoom;
class QgsRectangle;
class QgsPointXY;
class QgsCoordinateReferenceSystem;

/**
 * @brief QGIS map canvas wrapper (presentation layer)
 *
 * Wraps QgsMapCanvas. Signals carry user interaction events outward;
 * slots accept commands from the controller/service layer to update the view.
 * Contains no business logic — CRS selection, extent calculation, etc.
 * are decisions made upstream.
 */
class MapCanvasWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MapCanvasWidget(QWidget* parent = nullptr);

    QgsMapCanvas* mapCanvas() const;

signals:
    void canvasExtentChanged(const QgsRectangle& extent);
    void mapClicked(const QgsPointXY& point);
    void mapRightClicked(const QgsPointXY& point);

public slots:
    void setCanvasExtent(const QgsRectangle& extent);
    void setCanvasCrs(const QgsCoordinateReferenceSystem& crs);
    void setCanvasColor(const QColor& color);
    void refreshCanvas();

private slots:
    void onExtentChanged();

private:
    void setupUI();
    void setupMapTools();

    QgsMapCanvas* mCanvas = nullptr;
    QgsMapToolPan* mPanTool = nullptr;
    QgsMapToolZoom* mZoomTool = nullptr;
};

#endif // MAPCANVASWIDGET_H
