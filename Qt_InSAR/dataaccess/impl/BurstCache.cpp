#include "BurstCache.h"
#include "GdalSlcReader.h"

bool BurstCache::load(const QString& slcPath, int x, int y, int w, int h)
{
    clear();
    GdalSlcReader reader;
    if (!reader.open(slcPath)) return false;

    mData = reader.readBandWindow(0, x, y, w, h);
    reader.close();

    if (mData.size() < w * h) {
        clear();
        return false;
    }
    mWidth = w;
    mHeight = h;
    return true;
}

bool BurstCache::getWindow(int x, int y, int winW, int winH,
                            std::complex<float>* dst) const
{
    if (!isLoaded()) return false;
    if (x < 0 || y < 0 || x + winW > mWidth || y + winH > mHeight)
        return false;

    for (int row = 0; row < winH; ++row) {
        const auto* src = mData.constData() + (y + row) * mWidth + x;
        std::memcpy(dst + row * winW, src, winW * sizeof(std::complex<float>));
    }
    return true;
}
