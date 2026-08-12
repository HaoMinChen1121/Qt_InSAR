#include "TileCache.h"
#include <QtGlobal>

uint qHash(const TileCache::Key& k, uint seed)
{
    return qHash(k.bandKey, seed) ^ qHash(k.tx) ^ qHash(k.ty);
}

TileCache::TileCache(uint64_t maxBytes)
    : mMaxBytes(maxBytes)
{
}

std::shared_ptr<const TileCache::Tile> TileCache::get(const QString& bandKey,
                                                      int tx, int ty)
{
    QMutexLocker lock(&mMutex);
    Key k{bandKey, tx, ty};
    auto it = mMap.find(k);
    if (it == mMap.end()) return nullptr;
    // 移到 LRU 尾 (最近使用)
    mLru.removeOne(k);
    mLru.append(k);
    return it.value();
}

void TileCache::put(const QString& bandKey, int tx, int ty,
                    std::shared_ptr<Tile> tile)
{
    if (!tile) return;
    uint64_t tileBytes = static_cast<uint64_t>(tile->data.size()) * sizeof(float);

    QMutexLocker lock(&mMutex);
    Key k{bandKey, tx, ty};
    auto it = mMap.find(k);
    if (it != mMap.end()) {
        mLru.removeOne(k);
        mBytes -= static_cast<uint64_t>(it.value()->data.size()) * sizeof(float);
        mMap.erase(it);
    }
    mMap.insert(k, tile);
    mLru.append(k);
    mBytes += tileBytes;

    while (mBytes > mMaxBytes && !mLru.isEmpty()) {
        Key evict = mLru.takeFirst();
        auto ev = mMap.find(evict);
        if (ev != mMap.end()) {
            mBytes -= static_cast<uint64_t>(ev.value()->data.size()) * sizeof(float);
            mMap.erase(ev);
        }
    }
}

uint64_t TileCache::bytes() const
{
    QMutexLocker lock(&mMutex);
    return mBytes;
}

void TileCache::setMaxBytes(uint64_t maxBytes)
{
    QMutexLocker lock(&mMutex);
    mMaxBytes = maxBytes;
    while (mBytes > mMaxBytes && !mLru.isEmpty()) {
        Key evict = mLru.takeFirst();
        auto ev = mMap.find(evict);
        if (ev != mMap.end()) {
            mBytes -= static_cast<uint64_t>(ev.value()->data.size()) * sizeof(float);
            mMap.erase(ev);
        }
    }
}

void TileCache::clear()
{
    QMutexLocker lock(&mMutex);
    mMap.clear();
    mLru.clear();
    mBytes = 0;
}
