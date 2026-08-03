#ifndef RASTERRENDERER_H
#define RASTERRENDERER_H

#include <QString>

class QgsRasterLayer;

class RasterRenderer
{
public:
    /// Auto-detect layer type from name and apply appropriate renderer
    /// _phase → cyclic pseudo-color, _coh → [0,1] gray, else → 3σ-clip gray
    static void applyAutoRenderer(QgsRasterLayer* layer, const QString& layerName);

private:
    RasterRenderer() = delete;

    static void applyAmplitudeGray(QgsRasterLayer* layer);
    static void applyPhaseCyclic(QgsRasterLayer* layer);
    static void applyCoherenceGray(QgsRasterLayer* layer);
};

#endif // RASTERRENDERER_H
