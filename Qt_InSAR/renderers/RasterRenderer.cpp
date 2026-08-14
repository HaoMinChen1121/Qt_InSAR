#include "RasterRenderer.h"

#include "Sentinel1RasterProvider.h"
#include "dataaccess/impl/SentinelZipProduct.h"
#include "dataaccess/impl/TiffStreamDecoder.h"

#include <qgsrasterlayer.h>
#include <qgssinglebandgrayrenderer.h>
#include <qgssinglebandpseudocolorrenderer.h>
#include <qgsmultibandcolorrenderer.h>
#include <qgsrastershader.h>
#include <qgsrasterbandstats.h>
#include <qgscontrastenhancement.h>
#include <qgscolorrampshader.h>

#include <QColor>
#include <QPointer>
#include <QtGlobal>
#include <cmath>

static const double kPi = 3.14159265358979323846;

// ----

void RasterRenderer::applyAutoRenderer(QgsRasterLayer* layer,
                                        const QString& layerName)
{
    if (!layer || !layer->dataProvider()) return;

    if (layerName.contains(QStringLiteral("_color")))
        applyColorRgb(layer);
    else if (layerName.contains(QStringLiteral("_phase")))
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
    QgsSingleBandGrayRenderer* renderer =
        new QgsSingleBandGrayRenderer(provider, band);

    QgsContrastEnhancement* ce =
        new QgsContrastEnhancement(provider->dataType(band));
    ce->setContrastEnhancementAlgorithm(
        QgsContrastEnhancement::StretchToMinimumMaximum);
    renderer->setContrastEnhancement(ce);
    layer->setRenderer(renderer);

    // sentinel1zip: 统计计算在后台线程 (解码需 1~3s, 不能阻塞 UI)
    // 初始用 annotation 场景统计占位, 真实统计就绪后更新拉伸并重绘
    if (auto* sp = dynamic_cast<Sentinel1RasterProvider*>(provider)) {
        std::shared_ptr<TiffStreamDecoder> decoder = sp->bandDecoder();
        QPointer<QgsRasterLayer> layerPtr(layer);

        double pMin = 0, pMax = 0, pMean = 0, pStd = 0;
        if (decoder && decoder->sampledPresetStats(pMin, pMax, pMean, pStd)) {
            ce->setMinimumValue(pMin);
            ce->setMaximumValue(pMax);
        }

        insarbg::pool()->start(insarbg::makeRunnable([decoder, layerPtr]() {
            if (!decoder) return;
            decoder->ensureStats();   // 后台解码直至统计累积 (与预热共享缓存)
            double mn = 0, mx = 0, mean = 0, stdDev = 0;
            if (!decoder->sampledStats(mn, mx, mean, stdDev, 256, nullptr))
                return;
            double clipMin = qMax(mn, mean - 3.0 * stdDev);
            double clipMax = qMin(mx, mean + 3.0 * stdDev);
            QMetaObject::invokeMethod(layerPtr.data(),
                [layerPtr, clipMin, clipMax]() {
                    if (!layerPtr) return;
                    QgsSingleBandGrayRenderer* r =
                        dynamic_cast<QgsSingleBandGrayRenderer*>(layerPtr->renderer());
                    const QgsContrastEnhancement* old = r ? r->contrastEnhancement() : nullptr;
                    if (!r || !old) return;
                    QgsContrastEnhancement* newCe =
                        new QgsContrastEnhancement(*old);
                    newCe->setMinimumValue(clipMin);
                    newCe->setMaximumValue(clipMax);
                    r->setContrastEnhancement(newCe);   // 接管所有权
                    layerPtr->triggerRepaint();
                });
        }));
        return;
    }

    // 通用 GDAL 栅格: 原同步路径 (GDAL 统计快速)
    QgsRasterBandStats stats = provider->bandStatistics(
        band,
        Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max |
            Qgis::RasterBandStatistic::Mean | Qgis::RasterBandStatistic::StdDev,
        QgsRectangle(), 0);

    double clipMin = qMax(stats.minimumValue,
                          stats.mean - 3.0 * stats.stdDev);
    double clipMax = qMin(stats.maximumValue,
                          stats.mean + 3.0 * stats.stdDev);
    ce->setMinimumValue(clipMin);
    ce->setMaximumValue(clipMax);
}

// ----

void RasterRenderer::applyColorRgb(QgsRasterLayer* layer)
{
    // visualization/ *_color.tif: 3-band Byte RGB 直读
    QgsRasterDataProvider* provider = layer->dataProvider();
    if (!provider) return;
    QgsMultiBandColorRenderer* renderer =
        new QgsMultiBandColorRenderer(provider, 1, 2, 3);
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
