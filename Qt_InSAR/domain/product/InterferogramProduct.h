#ifndef INTERFEROGRAMPRODUCT_H
#define INTERFEROGRAMPRODUCT_H

#include "domain/QsarProduct.h"
#include "dataaccess/impl/QsarIO.h"

// Interferogram 产品: interferogram.qsar 的类型化薄包装
// 不复制数据 — qsar (QsarProduct) 是唯一事实源
// 下游 (Filter/Unwrap/Deformation/Geocoding) 只认此对象, 不找文件路径
class InterferogramProduct
{
public:
    InterferogramProduct() = default;
    explicit InterferogramProduct(const QsarProduct& p) : mProduct(p) {}

    static InterferogramProduct load(const QString& qsarPath)
    { return InterferogramProduct(QsarIO::read(qsarPath)); }

    bool isValid() const { return !mProduct.bands.isEmpty(); }
    const QsarProduct& raw() const { return mProduct; }
    const ProductMetadata& metadata() const { return mProduct.metadata; }

    // ── 波段访问 (合并产品 subSwath=="IW" 优先, 无合并时取首波段) ──
    const QsarBand* mergedBand(const QString& polarization = QString()) const
    {
        const QsarBand* fallback = nullptr;
        for (const auto& b : mProduct.bands) {
            if (!polarization.isEmpty() && b.polarization != polarization) continue;
            if (b.subSwath == QStringLiteral("IW")) return &b;
            if (!fallback) fallback = &b;
        }
        return fallback;
    }

    QString complexIfg(const QString& polarization = QString()) const
    { const QsarBand* b = mergedBand(polarization); return b ? b->ifgFile : QString(); }

    QString coherence(const QString& polarization = QString()) const
    { const QsarBand* b = mergedBand(polarization); return b ? b->cohFile : QString(); }

    QString phase(const QString& polarization = QString()) const
    { const QsarBand* b = mergedBand(polarization); return b ? b->phaseFile : QString(); }

    // PhaseCorrection 语义: diff 即 corrected interferogram
    QString correctedIfg(const QString& polarization = QString()) const
    { const QsarBand* b = mergedBand(polarization); return b ? b->diffFile : QString(); }

    QString correctedPhase(const QString& polarization = QString()) const
    { const QsarBand* b = mergedBand(polarization); return b ? b->diffPhaseFile : QString(); }

    QStringList polarizations() const
    {
        QStringList pols;
        for (const auto& b : mProduct.bands) {
            if (!b.polarization.isEmpty() && !pols.contains(b.polarization))
                pols.append(b.polarization);
        }
        return pols;
    }

    // ── 类型化访问器 (metadata, schema v2.0) ──
    const QsarPairInfo& pair() const { return mProduct.metadata.pair; }
    const QsarBaselineMeta& baseline() const { return mProduct.metadata.baseline; }
    const QsarGeometryMeta& geometry() const { return mProduct.metadata.geometry; }
    const QsarQualityMeta& quality() const { return mProduct.metadata.quality; }

    QString orbitDirection() const
    {
        if (!mProduct.metadata.orbitMaster.direction.isEmpty())
            return mProduct.metadata.orbitMaster.direction;
        return mProduct.metadata.orbitSlave.direction;
    }

    int rangeLooks() const
    { return mProduct.metadata.processing.interferogram.rangeLooks; }

    int azimuthLooks() const
    { return mProduct.metadata.processing.interferogram.azimuthLooks; }

    double wavelength() const
    { return mProduct.metadata.processing.interferogram.wavelength; }

    // 像元间距 (米), 下游窗口参数/解缠/形变直接取用
    double rangeSpacing() const
    {
        double s = mProduct.metadata.processing.interferogram.outputRangeSpacing;
        if (s <= 0) s = mProduct.metadata.geometry.farRange > 0
            ? (mProduct.metadata.geometry.farRange - mProduct.metadata.geometry.nearRange)
              / qMax(1, mProduct.bands.value(0).width - 1)
            : 0.0;
        return s;
    }

private:
    QsarProduct mProduct;
};

#endif // INTERFEROGRAMPRODUCT_H
