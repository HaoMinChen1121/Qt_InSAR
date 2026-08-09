#include "SentinelDataReader.h"
#include "domain/SarComplexTypes.h"
#include <gdal_priv.h>
#include <cpl_vsi.h>
#include <cpl_conv.h>
#include <QDebug>
#include <cstdint>
#include <cstring>

// ── 最小 TIFF IFD 解析: 仅提取 StripOffsets / StripByteCounts 首值 ──
static bool parseTiffStripOffset(const uint8_t* data, size_t dataSize,
    uint32_t* outFirstStripOff, uint32_t* outStripByteCount)
{
    if (dataSize < 8) return false;

    bool le;
    if      (data[0] == 'I' && data[1] == 'I') le = true;
    else if (data[0] == 'M' && data[1] == 'M') le = false;
    else return false;

    auto r16 = [le](const uint8_t* p) -> uint16_t {
        return le ? (p[0] | (static_cast<uint16_t>(p[1]) << 8))
                  : ((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    };
    auto r32 = [le](const uint8_t* p) -> uint32_t {
        return le
            ? (p[0] | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
            : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
            | (static_cast<uint32_t>(p[2]) << 8) | p[3]);
    };

    if (r16(data + 2) != 42) return false;

    uint32_t ifdOff = r32(data + 4);
    if (ifdOff + 2 > dataSize) return false;

    uint16_t numTags = r16(data + ifdOff);
    const uint8_t* tp = data + ifdOff + 2;

    // 辅助: 读取 IFD tag 的值 (若 count*typeSize>4, val是偏移; 否则val即值)
    auto tagVal = [&](const uint8_t* tp, uint32_t* countOut) -> uint32_t {
        uint16_t type  = r16(tp + 2);
        uint32_t count = r32(tp + 4);
        uint32_t val   = r32(tp + 8);
        if (countOut) *countOut = count;
        // SHORT=3 (2B), LONG=4 (4B), etc.
        uint32_t typeSize = (type == 3) ? 2 : (type == 4) ? 4 : 1;
        if (count * typeSize <= 4)
            return val;                    // 值内联在 tp+8
        else if (val + count * typeSize <= static_cast<uint32_t>(dataSize))
            return r32(data + val);        // val 是偏移, 取首个元素
        return 0;
    };

    uint32_t stripOff = 0, stripBC = 0;
    for (int i = 0; i < numTags; ++i, tp += 12) {
        if (tp + 12 > data + dataSize) return false;
        uint16_t tag = r16(tp);
        if (tag == 273)          // StripOffsets
            stripOff = tagVal(tp, nullptr);
        else if (tag == 279)     // StripByteCounts
            stripBC = tagVal(tp, nullptr);
        if (stripOff && stripBC) break;
    }
    if (!stripOff || !stripBC) return false;

    *outFirstStripOff = stripOff;
    *outStripByteCount = stripBC;
    return true;
}

SentinelDataReader::~SentinelDataReader() { close(); }

bool SentinelDataReader::open(const QString& vsiPath, const SarBandDescriptor& band)
{
    close();

    // ═══ Step 1: 将 ZIP 内 TIFF 一次性读入 mRawTiff ═══
    VSILFILE* fp = VSIFOpenExL(vsiPath.toUtf8().constData(), "rb", TRUE);
    if (!fp) {
        qWarning() << "[SDR] Cannot open VSI file:" << vsiPath;
        return false;
    }
    VSIStatBufL st;
    if (VSIStatL(vsiPath.toUtf8().constData(), &st) != 0 || st.st_size <= 0) {
        qWarning() << "[SDR] Cannot stat VSI file:" << vsiPath;
        VSIFCloseL(fp);
        return false;
    }
    mRawTiff.resize(static_cast<size_t>(st.st_size));
    vsi_l_offset totalRead = 0, rem = static_cast<vsi_l_offset>(st.st_size);
    while (rem > 0) {
        vsi_l_offset n = VSIFReadL(mRawTiff.data() + totalRead, 1, rem, fp);
        if (n == 0) break;
        totalRead += n; rem -= n;
    }
    VSIFCloseL(fp);
    if (totalRead < static_cast<vsi_l_offset>(st.st_size)) {
        qWarning() << "[SDR] Incomplete VSI read" << totalRead << "/" << st.st_size;
        return false;
    }

    // ═══ Step 2: /vsimem/ 仅用于 GDAL 元数据 (宽/高/地理参考) ═══
    mMemPath = QString("/vsimem/_sdr_%1.tif").arg(
        reinterpret_cast<quintptr>(this), 0, 16);
    VSIFCloseL(VSIFileFromMemBuffer(mMemPath.toUtf8().constData(),
        mRawTiff.data(), st.st_size, FALSE));

    mVsiDataset = GDALOpenShared(mMemPath.toUtf8().constData(), GA_ReadOnly);
    if (!mVsiDataset) {
        qWarning() << "[SDR] Failed to open /vsimem/ TIFF";
        VSIUnlink(mMemPath.toUtf8().constData());
        mMemPath.clear();
        return false;
    }
    mWidth  = GDALGetRasterXSize(static_cast<GDALDatasetH>(mVsiDataset));
    mHeight = GDALGetRasterYSize(static_cast<GDALDatasetH>(mVsiDataset));

    mBurstCount    = band.burstCount;
    mLinesPerBurst = band.linesPerBurst;
    mBurstStartLines = band.burstStartLines;
    mFmRate        = band.azimuthFmRate;
    mSteeringRate  = band.azimuthSteeringRate;
    mPrf           = band.azimuthFrequency;

    if (mBurstCount <= 0) return true;

    // ═══ Step 3: 解析 TIFF IFD, 获取首个 strip 偏移和大小 ═══
    uint32_t firstStripOff = 0, stripByteCount = 0;
    if (!parseTiffStripOffset(mRawTiff.data(), mRawTiff.size(),
            &firstStripOff, &stripByteCount)) {
        qWarning() << "[SDR] Failed to parse TIFF strip offsets";
        close();
        return false;
    }

    // ═══ Step 4: 直接从 mRawTiff 读取 strip 数据, CInt16→SoA, 切分 burst ═══
    //  跳过 GDAL RasterIO + fullBuf CFloat32 中间缓冲
    mCaches.resize(mBurstCount);
    for (int b = 0; b < mBurstCount; ++b) {
        int row0 = mBurstStartLines.value(b, b * mLinesPerBurst);
        int burstH = mLinesPerBurst;
        if (b == mBurstCount - 1)
            burstH = mHeight - row0;
        if (burstH <= 0 || row0 < 0 || row0 + burstH > mHeight) {
            qWarning() << "[SDR] Invalid burst bounds" << b;
            continue;
        }

        // 定位该 burst 的 strip 起始位置
        size_t burstOff = static_cast<size_t>(firstStripOff)
                        + static_cast<size_t>(row0) * stripByteCount;
        if (burstOff + static_cast<size_t>(burstH) * stripByteCount > mRawTiff.size()) {
            qWarning() << "[SDR] Strip data out of bounds burst" << b;
            continue;
        }

        mCaches[b].loadFromRawStrips(mRawTiff.data() + burstOff, mWidth, burstH);

        if (mPrf > 0.0 && std::abs(mFmRate) > 1e-6)
            mCaches[b].applyDeramp(mPrf, mFmRate, row0, b);
    }

    qDebug() << "[SDR] Preloaded" << mBurstCount << "bursts" << mWidth << "x" << mHeight
             << "strips" << stripByteCount << "bytes/row";
    return true;
}

void SentinelDataReader::close()
{
    mCaches.clear();
    if (mVsiDataset) {
        GDALClose(static_cast<GDALDatasetH>(mVsiDataset));
        mVsiDataset = nullptr;
    }
    if (!mMemPath.isEmpty()) {
        VSIUnlink(mMemPath.toUtf8().constData());
        mMemPath.clear();
    }
    mWidth = mHeight = 0;
    mBurstCount = mLinesPerBurst = 0;
    mBurstStartLines.clear();
    mVsiPath.clear();
    mRawTiff.clear();
    mRawTiff.shrink_to_fit();
}

sar::ComplexSoAView SentinelDataReader::burstSoaView(int idx)
{
    sar::ComplexSoAView empty{};
    if (idx < 0 || idx >= mBurstCount) return empty;
    if (!mCaches[idx].isLoaded()) return empty;
    return mCaches[idx].soaView();
}

bool SentinelDataReader::readWindow(int x0, int y0, int w, int h, std::complex<float>* dst)
{
    if (!dst || w <= 0 || h <= 0) return false;
    if (x0 < 0 || y0 < 0 || x0 + w > mWidth || y0 + h > mHeight) return false;
    if (mLinesPerBurst <= 0 || mBurstCount <= 0) return false;

    std::fill(dst, dst + static_cast<size_t>(w) * h, std::complex<float>(0.0f, 0.0f));

    int burstFirst = y0 / mLinesPerBurst;
    int burstLast  = (y0 + h - 1) / mLinesPerBurst;
    if (burstFirst < 0) burstFirst = 0;
    if (burstLast >= mBurstCount) burstLast = mBurstCount - 1;

    for (int bi = burstFirst; bi <= burstLast; ++bi) {
        if (!mCaches[bi].isLoaded()) continue;

        int burstRow0 = mBurstStartLines.value(bi, bi * mLinesPerBurst);
        int burstH    = (bi == mBurstCount - 1)
                        ? mHeight - burstRow0 : mLinesPerBurst;

        int interY0 = std::max(y0, burstRow0);
        int interY1 = std::min(y0 + h, burstRow0 + burstH);
        if (interY0 >= interY1) continue;

        int srcX = x0;
        int srcY = interY0 - burstRow0;
        int copyW = w;
        int copyH = interY1 - interY0;
        int dstOff = (interY0 - y0) * w;

        mCaches[bi].getWindow(srcX, srcY, copyW, copyH, dst + dstOff);
    }
    return true;
}
