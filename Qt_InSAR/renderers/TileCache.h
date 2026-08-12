#ifndef TILECACHE_H
#define TILECACHE_H

#include <QString>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QVector>
#include <memory>

// 线程安全 LRU Tile 缓存: 256x256 Float32 (振幅/实值) 块
class TileCache {
public:
    static constexpr int kTileSize = 256;

    struct Tile {
        int width = 0;              // 边缘 tile 可能小于 256
        int height = 0;
        QVector<float> data;        // width*height
    };

    // 默认 2GB (IW SLC 全分辨率 tile 约 1.16GB/波段)
    explicit TileCache(uint64_t maxBytes = 2ull * 1024 * 1024 * 1024);

    // 命中返回共享指针; 未命中返回 nullptr
    // (非 const: 命中时更新 LRU 序)
    std::shared_ptr<const Tile> get(const QString& bandKey, int tx, int ty);
    void put(const QString& bandKey, int tx, int ty, std::shared_ptr<Tile> tile);

    uint64_t bytes() const;
    void setMaxBytes(uint64_t maxBytes);
    void clear();

private:
    struct Key {
        QString bandKey;
        int tx = 0;
        int ty = 0;
        bool operator==(const Key& o) const
        { return tx == o.tx && ty == o.ty && bandKey == o.bandKey; }
    };
    friend uint qHash(const TileCache::Key& k, uint seed);

    mutable QMutex mMutex;
    QHash<Key, std::shared_ptr<Tile>> mMap;
    QList<Key> mLru;                // 头=最久未用
    uint64_t mMaxBytes;
    uint64_t mBytes = 0;
};

#endif // TILECACHE_H
