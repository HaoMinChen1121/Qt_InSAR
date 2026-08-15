#ifndef PRODUCTMANAGER_H
#define PRODUCTMANAGER_H

#include <QHash>
#include "domain/QsarProduct.h"
#include "dataaccess/impl/QsarIO.h"
#include "domain/product/InterferogramProduct.h"
#include "domain/product/RegisteredSLCProduct.h"

// 内存产品注册表 (以 qsar 绝对路径为 id)
// 下游服务只认 productId, 经此解析为类型化产品 — 不直接找文件路径
class ProductManager
{
public:
    bool registerProduct(const QString& qsarPath)
    {
        QsarProduct p = QsarIO::read(qsarPath);
        if (p.bands.isEmpty()) return false;
        mProducts.insert(qsarPath, p);
        return true;
    }

    void unregister(const QString& qsarPath) { mProducts.remove(qsarPath); }
    bool contains(const QString& qsarPath) const { return mProducts.contains(qsarPath); }
    QStringList registeredPaths() const { return mProducts.keys(); }

    InterferogramProduct interferogram(const QString& qsarPath) const
    { return InterferogramProduct(mProducts.value(qsarPath)); }

    RegisteredSLCProduct registeredSLC(const QString& qsarPath) const
    { return RegisteredSLCProduct(mProducts.value(qsarPath)); }

private:
    QHash<QString, QsarProduct> mProducts;
};

#endif // PRODUCTMANAGER_H
