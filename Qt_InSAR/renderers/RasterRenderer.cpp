#include "RasterRenderer.h"

#include <qgsrasterlayer.h>
#include <qgssinglebandgrayrenderer.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsrastershader.h>
#include <qgsrasterbandstats.h>
#include <qgscontrastenhancement.h>
#include <qgscolorrampshader.h>

#include <QColor>
#include <QtGlobal>
#include <cmath>

static const double kPi = 3.14159265358979323846;

// ----

void RasterRenderer::applyAutoRenderer(QgsRasterLayer* layer,
                                        const QString& layerName)
{
    if (!layer || !layer->dataProvider()) return;

    if (layerName.contains(QStringLiteral("_phase")))
        applyPhaseCyclic(layer);
    else if (layerName.contains(QStringLiteral("_coh")))
        applyCoherenceGray(layer);
    else
        applyAmplitudeGray(layer);
}

// ----

void RasterRenderer::applyAmplitudeGray(QgsRasterLayer* layer)
{
    QgsRasterDataProvider* provider = layer->dataProvider();
    if (!provider) return;

    int band = 1;
    QgsRasterBandStats stats = provider->bandStatistics(
        band,
        Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max |
            Qgis::RasterBandStatistic::Mean | Qgis::RasterBandStatistic::StdDev,
        QgsRectangle(), 0);

    double clipMin = qMax(stats.minimumValue,
                          stats.mean - 3.0 * stats.stdDev);
    double clipMax = qMin(stats.maximumValue,
                          stats.mean + 3.0 * stats.stdDev);

    QgsSingleBandGrayRenderer* renderer =
        new QgsSingleBandGrayRenderer(provider, band);

    QgsContrastEnhancement* ce =
        new QgsContrastEnhancement(provider->dataType(band));
    ce->setContrastEnhancementAlgorithm(
        QgsContrastEnhancement::StretchToMinimumMaximum);
    ce->setMinimumValue(clipMin);
    ce->setMaximumValue(clipMax);
    renderer->setContrastEnhancement(ce);

    layer->setRenderer(renderer);
}

// ----

void RasterRenderer::applyPhaseCyclic(QgsRasterLayer* layer)
{
    QgsRasterDataProvider* provider = layer->dataProvider();
    if (!provider) return;

    int band = 1;
    QgsRasterBandStats stats = provider->bandStatistics(
        band, Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max,
        QgsRectangle(), 0);

    double pMin = stats.minimumValue;
    double pMax = stats.maximumValue;
    double pHalf     = (pMin + pMax) * 0.5;
    double pQuarter1 = pMin + (pMax - pMin) * 0.25;
    double pQuarter3 = pMin + (pMax - pMin) * 0.75;

    // Cyclic ramp blue → cyan → white → red → blue (end matches start)
    QgsColorRampShader* shaderFunc = new QgsColorRampShader(pMin, pMax);
    shaderFunc->setColorRampType(Qgis::ShaderInterpolationMethod::Linear);
    shaderFunc->setColorRampItemList({
        QgsColorRampShader::ColorRampItem(pMin,      QColor(0,   0,   255)),
        QgsColorRampShader::ColorRampItem(pQuarter1, QColor(0,   255, 255)),
        QgsColorRampShader::ColorRampItem(pHalf,     QColor(255, 255, 255)),
        QgsColorRampShader::ColorRampItem(pQuarter3, QColor(255, 0,   0)),
        QgsColorRampShader::ColorRampItem(pMax,      QColor(0,   0,   255)),
    });

    QgsRasterShader* shader = new QgsRasterShader();
    shader->setRasterShaderFunction(shaderFunc);

    QgsSingleBandPseudoColorRenderer* renderer =
        new QgsSingleBandPseudoColorRenderer(provider, band, shader);

    layer->setRenderer(renderer);
}

// ----

void RasterRenderer::applyCoherenceGray(QgsRasterLayer* layer)
{
    QgsRasterDataProvider* provider = layer->dataProvider();
    if (!provider) return;

    int band = 1;
    QgsSingleBandGrayRenderer* renderer =
        new QgsSingleBandGrayRenderer(provider, band);

    QgsContrastEnhancement* ce =
        new QgsContrastEnhancement(provider->dataType(band));
    ce->setContrastEnhancementAlgorithm(
        QgsContrastEnhancement::StretchToMinimumMaximum);
    ce->setMinimumValue(0.0);
    ce->setMaximumValue(1.0);
    renderer->setContrastEnhancement(ce);

    layer->setRenderer(renderer);
}
