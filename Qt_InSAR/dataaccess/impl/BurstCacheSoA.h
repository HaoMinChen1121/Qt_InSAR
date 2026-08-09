#ifndef BURSTCACHESOA_H
#define BURSTCACHESOA_H

#include "algorithms/ComplexSoA.h"
#include "domain/SarComplexTypes.h"
#include <complex>
#include <QVector>

class BurstCacheSoA {
public:
    BurstCacheSoA() = default;
    BurstCacheSoA(BurstCacheSoA&&) = default;
    BurstCacheSoA& operator=(BurstCacheSoA&&) = default;

    // 从 GDAL 数据集直接读入 burst 行范围数据 (CInt16→CFloat32→SoA)
    bool loadBurst(void* hDS, int band, int col0, int row0, int w, int h);

    // 从已完整读取的 CFloat32 缓冲区中提取指定行范围的 SoA 数据 (预加载路径)
    void loadFromCfloat32(const CFloat32* src, int w, int h);

    // 从原始 CInt16 strip 数据直接转换为 SoA (跳过 GDAL RasterIO)
    // src 指向 row0 的第一个像素: int16 交织对 {re, im, re, im, ...}
    void loadFromRawStrips(const void* src, int w, int h);

    // 原地 TOPS deramp
    void applyDeramp(double prf, double kt, int burstRow0, int burstIdx);

    // 零拷贝视图 (给 SincResampler SoA 路径)
    sar::ComplexSoAView soaView() const;

    // SoA→AoS 拷贝 (给 correlator/ESD 等 AoS 消费者)
    bool getWindow(int x, int y, int w, int h, std::complex<float>* dst) const;
    QVector<std::complex<float>> getWindow(int x, int y, int w, int h) const;

    bool isLoaded() const { return mLoaded; }
    void clear();
    int width() const { return mWidth; }
    int height() const { return mHeight; }

private:
    sar::ComplexSoA mData;
    int mWidth = 0;
    int mHeight = 0;
    bool mLoaded = false;
};

#endif // BURSTCACHESOA_H
