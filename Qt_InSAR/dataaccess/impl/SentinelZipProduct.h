#ifndef SENTINELZIPPRODUCT_H
#define SENTINELZIPPRODUCT_H

#include "ZipStore.h"
#include "ZipTiffExtractor.h"
#include "TiffStreamDecoder.h"
#include "renderers/TileCache.h"
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMutex>
#include <QThreadPool>
#include <QRunnable>
#include <memory>
#include <utility>

// 专用后台线程池 (预热/统计解码), 与 QGIS 渲染的全局线程池隔离
namespace insarbg {
QThreadPool* pool();

template <typename F>
class FunctorRunnable : public QRunnable {
public:
    explicit FunctorRunnable(F f) : mF(std::move(f)) {}
    void run() override { mF(); }
private:
    F mF;
};

template <typename F>
FunctorRunnable<F>* makeRunnable(F f)
{
    return new FunctorRunnable<F>(std::move(f));
}
} // namespace insarbg

// 对应 SNAP Product: 管理一个 Sentinel-1 ZIP 产品
// (持久 ZIP 句柄 + 每 band 的 TIFF 元数据/解码器惰性缓存)
class SentinelZipProduct {
public:
    static std::shared_ptr<SentinelZipProduct> open(const QString& zipPath);

    std::shared_ptr<ZipStore> zipStore() const { return mStore; }
    QString zipPath() const { return mZipPath; }

    // ZIP 内 measurement 目录的 TIFF entries
    QStringList measurementEntries() const { return mMeasurementEntries; }

    // 惰性解析 TIFF 头 (仅 inflate 头部 ~100KB), 结果缓存
    std::shared_ptr<const TiffHeaderInfo> bandTiffInfo(const QString& entry) const;

    // 惰性创建解码器 (共享 TileCache)
    std::shared_ptr<TiffStreamDecoder> bandDecoder(const QString& entry) const;

private:
    SentinelZipProduct() = default;
    void discoverMeasurementEntries();
    // 假定 mMutex 已持有
    std::shared_ptr<const TiffHeaderInfo> bandTiffInfoLocked(const QString& entry) const;
    // 从 annotation XML 构建 TIFF 元数据 (S1 主路径: 尺寸/样本格式/strip起始/地理网格)
    bool buildFromAnnotation(const QString& entry, TiffHeaderInfo* out) const;

    QString mZipPath;
    std::shared_ptr<ZipStore> mStore;
    std::shared_ptr<TileCache> mTileCache;   // 同产品各 band 共享
    QStringList mMeasurementEntries;

    mutable QMutex mMutex;
    mutable QHash<QString, std::shared_ptr<const TiffHeaderInfo>> mTiffInfoCache;
    mutable QHash<QString, std::shared_ptr<TiffStreamDecoder>> mDecoderCache;
};

// 全局产品注册表: 引用计数管理产品生命周期
class SentinelProductManager {
public:
    static SentinelProductManager& instance();

    std::shared_ptr<SentinelZipProduct> acquire(const QString& zipPath);
    void release(const QString& zipPath);

    // 关闭时清空
    void clear();

private:
    SentinelProductManager() = default;

    QMutex mMutex;
    QHash<QString, std::shared_ptr<SentinelZipProduct>> mProducts;
    QHash<QString, int> mRefCounts;
};

#endif // SENTINELZIPPRODUCT_H
