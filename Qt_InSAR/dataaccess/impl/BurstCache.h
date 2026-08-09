#ifndef BURSTCACHE_H
#define BURSTCACHE_H

#include <QString>
#include <QVector>
#include <complex>
#include <cstring>

// ── Burst级SLC数据内存缓存 ──
// 从TIFF一次读入burst行范围, 后续窗口全部从内存memcpy提取,
// 彻底消除GDAL RasterIO随机读和ZIP解压开销.
class BurstCache {
public:
    BurstCache() = default;

    // 从SLC文件读取指定行范围到内存. 返回false则缓存为空.
    bool load(const QString& slcPath, int x, int y, int w, int h);

    // 从缓存提取窗口. dst由调用者预分配(winW*winH).
    // 窗口超出缓存范围返回false.
    bool getWindow(int x, int y, int winW, int winH,
                   std::complex<float>* dst) const;

    const std::complex<float>* data() const { return mData.constData(); }
    int width()  const { return mWidth; }
    int height() const { return mHeight; }
    bool isLoaded() const { return !mData.isEmpty(); }
    void clear() { mData.clear(); mWidth = mHeight = 0; }

private:
    QVector<std::complex<float>> mData;
    int mWidth  = 0;
    int mHeight = 0;
};

#endif // BURSTCACHE_H
