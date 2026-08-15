#include "BurstCacheSoA.h"
#include "algorithms/DerampCore.h"
#include "domain/SarComplexTypes.h"
#include <gdal_priv.h>
#include <vector>

bool BurstCacheSoA::loadBurst(void* hDS, int band, int col0, int row0, int w, int h)
{
    if (!hDS || w <= 0 || h <= 0) return false;

    GDALDataset* ds = static_cast<GDALDataset*>(hDS);
    GDALRasterBand* rb = ds->GetRasterBand(band + 1);

    std::vector<CFloat32> buf(static_cast<size_t>(w) * h);
    CPLErr err = rb->RasterIO(GF_Read, col0, row0, w, h,
        buf.data(), w, h, GDT_CFloat32, 0, 0);
    if (err != CE_None) return false;

    mData.fromCfloat32(buf.data(), w * h);
    mWidth = w;
    mHeight = h;
    mLoaded = true;
    return true;
}

void BurstCacheSoA::loadFromCfloat32(const CFloat32* src, int w, int h)
{
    mData.fromCfloat32(src, w * h);
    mWidth = w;
    mHeight = h;
    mLoaded = true;
}

void BurstCacheSoA::loadFromRawStrips(const void* src, int w, int h)
{
    int n = w * h;
    mData.alloc(n);
    const int16_t* s = static_cast<const int16_t*>(src);
    for (int i = 0; i < n; ++i) {
        mData.re[i] = static_cast<float>(s[i * 2]);
        mData.im[i] = static_cast<float>(s[i * 2 + 1]);
    }
    mWidth = w;
    mHeight = h;
    mLoaded = true;
}

void BurstCacheSoA::applyDeramp(double prf, double kt, int burstRow0, int burstIdx,
                                double nearRange, double rangeSpacing,
                                double ktAnnotation)
{
    if (!mLoaded) return;
    sar::applyDeramp_SoA(mData, mWidth, mHeight, burstRow0, burstIdx, prf, kt,
                         nearRange, rangeSpacing, ktAnnotation);
}

sar::ComplexSoAView BurstCacheSoA::soaView() const
{
    return { mData.re, mData.im, mData.size };
}

bool BurstCacheSoA::getWindow(int x, int y, int w, int h, std::complex<float>* dst) const
{
    if (!mLoaded || !dst || w <= 0 || h <= 0) return false;
    if (x < 0 || y < 0 || x + w > mWidth || y + h > mHeight) return false;

    for (int row = 0; row < h; ++row) {
        int srcOff = (y + row) * mWidth + x;
        int dstOff = row * w;
        for (int col = 0; col < w; ++col) {
            dst[dstOff + col] = { mData.re[srcOff + col], mData.im[srcOff + col] };
        }
    }
    return true;
}

QVector<std::complex<float>> BurstCacheSoA::getWindow(int x, int y, int w, int h) const
{
    QVector<std::complex<float>> result(w * h);
    if (!getWindow(x, y, w, h, result.data()))
        result.clear();
    return result;
}

void BurstCacheSoA::clear()
{
    mData.free();
    mWidth = 0;
    mHeight = 0;
    mLoaded = false;
}
