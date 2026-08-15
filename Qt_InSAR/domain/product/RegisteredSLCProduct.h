#ifndef REGISTEREDSLCPRODUCT_H
#define REGISTEREDSLCPRODUCT_H

#include "domain/QsarProduct.h"
#include "dataaccess/impl/QsarIO.h"

// RegisteredSLC 产品: registered.qsar 的类型化薄包装
// 不复制数据 — qsar (QsarProduct) 是唯一事实源
class RegisteredSLCProduct
{
public:
    RegisteredSLCProduct() = default;
    explicit RegisteredSLCProduct(const QsarProduct& p) : mProduct(p) {}

    static RegisteredSLCProduct load(const QString& qsarPath)
    { return RegisteredSLCProduct(QsarIO::read(qsarPath)); }

    bool isValid() const { return !mProduct.bands.isEmpty(); }
    const QsarProduct& raw() const { return mProduct; }
    const ProductMetadata& metadata() const { return mProduct.metadata; }

    // ── 类型化访问器 (metadata, schema v2.0) ──
    const QsarPairInfo& pair() const { return mProduct.metadata.pair; }
    const QsarOrbitMeta& masterOrbit() const { return mProduct.metadata.orbitMaster; }
    const QsarOrbitMeta& slaveOrbit() const { return mProduct.metadata.orbitSlave; }
    const QsarBaselineMeta& baseline() const { return mProduct.metadata.baseline; }
    const QsarQualityMeta& quality() const { return mProduct.metadata.quality; }

    // 轨道方向 (master 优先, 缺省取 slave)
    QString orbitDirection() const
    {
        if (!mProduct.metadata.orbitMaster.direction.isEmpty())
            return mProduct.metadata.orbitMaster.direction;
        return mProduct.metadata.orbitSlave.direction;
    }

private:
    QsarProduct mProduct;
};

#endif // REGISTEREDSLCPRODUCT_H
