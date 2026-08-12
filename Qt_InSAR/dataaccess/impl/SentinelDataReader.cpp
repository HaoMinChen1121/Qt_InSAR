#include "SentinelDataReader.h"
#include "domain/SarComplexTypes.h"
#include <gdal_priv.h>
#include <cpl_vsi.h>
#include <QDebug>
#include <QFile>
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>

// ── zlib 最小定义 (不依赖 zlib.h, 动态加载 zlib.dll) ──
struct ZStream {
    const uint8_t* next_in  = nullptr;
    uint32_t       avail_in  = 0;
    unsigned long  total_in  = 0;
    uint8_t*       next_out  = nullptr;
    uint32_t       avail_out = 0;
    unsigned long  total_out = 0;
    const char*    msg       = nullptr;
    void*          state     = nullptr;
    void*          zalloc    = nullptr;
    void*          zfree     = nullptr;
    void*          opaque    = nullptr;
    int            data_type = 0;
    unsigned long  adler     = 0;
    unsigned long  reserved  = 0;
};
#define Z_OK          0
#define Z_STREAM_END  1
#define Z_FINISH      4
#define MAX_WBITS    15

typedef int (*InflateInit2_t)(ZStream*, int, const char*, int);
typedef int (*Inflate_t)(ZStream*, int);
typedef int (*InflateEnd_t)(ZStream*);

static InflateInit2_t p_InflateInit2_;
static Inflate_t      p_Inflate;
static InflateEnd_t   p_InflateEnd;
static bool            sZlibLoaded = false;

static bool loadZlibOnce()
{
    if (sZlibLoaded) return p_Inflate != nullptr;
    sZlibLoaded = true;

    // zlib.dll 可能还没加载, 但 GDAL 已将其加载到进程空间
    HMODULE h = GetModuleHandleW(L"zlib.dll");
    if (!h) h = GetModuleHandleW(L"zlibwapi.dll");
    if (!h) h = LoadLibraryW(L"zlib.dll");
    if (!h) {
        qWarning() << "[SDR] Cannot load zlib.dll";
        return false;
    }
    p_InflateInit2_ = (InflateInit2_t)GetProcAddress(h, "inflateInit2_");
    p_Inflate       = (Inflate_t)     GetProcAddress(h, "inflate");
    p_InflateEnd    = (InflateEnd_t)  GetProcAddress(h, "inflateEnd");
    return p_Inflate != nullptr;
}

// ── ZIP 常量 ──
static constexpr uint32_t kEocdSig     = 0x06054b50;
static constexpr uint32_t kCdEntrySig  = 0x02014b50;
static constexpr uint16_t kMethodDeflate = 8;

// ── 从 ZIP 中读取并解压指定 entry ──
static std::vector<unsigned char> readZipEntry(const QString& zipPath,
                                                const QString& entryName)
{
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ZIP] Cannot open:" << zipPath;
        return {};
    }
    qint64 fsize = f.size();
    if (fsize < 22) return {};

    // 搜索 EOCD (从末尾向前 64KB)
    int searchLen = std::min(static_cast<int>(fsize), 65536 + 22);
    f.seek(fsize - searchLen);
    QByteArray tail = f.read(searchLen);

    int eocdIdx = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        uint32_t sig;
        std::memcpy(&sig, tail.constData() + i, 4);
        if (sig == kEocdSig) { eocdIdx = i; break; }
    }
    if (eocdIdx < 0) { qWarning() << "[ZIP] EOCD not found"; return {}; }

    // 直接从 tail buffer 读 EOCD 字段, 避免 seek 偏差
    const char* eocd = tail.constData() + eocdIdx;
    uint16_t cdDisk, cdCount;
    uint32_t cdSize;
    uint64_t cdOff;
    uint32_t cdOff32;
    std::memcpy(&cdDisk,   eocd + 8,  2);
    std::memcpy(&cdCount,  eocd + 10, 2);
    std::memcpy(&cdSize,   eocd + 12, 4);
    std::memcpy(&cdOff32,  eocd + 16, 4);
    cdOff = cdOff32;

    qDebug() << "[ZIP] EOCD disk=" << cdDisk << "count=" << cdCount
             << "cdSize=" << cdSize << "cdOff=" << cdOff;

    // ── ZIP64 处理: cdOff == 0xFFFFFFFF 表示需要 64 位偏移 ──
    if (cdOff == 0xFFFFFFFF || cdCount == 0xFFFF || cdSize == 0xFFFFFFFF) {
        // ZIP64 End of Central Directory Locator 在 EOCD 前 20 字节
        // 签名 0x07064b50, 后跟 8 字节 ZIP64 EOCDR 偏移
        if (eocdIdx < 20) { qWarning() << "[ZIP] EOCD too close to start for ZIP64"; return {}; }
        uint32_t zip64Sig;
        std::memcpy(&zip64Sig, tail.constData() + eocdIdx - 20, 4);
        if (zip64Sig != 0x07064b50) {
            qWarning() << "[ZIP] ZIP64 locator not found";
            return {};
        }
        uint64_t zip64EocdrOff;
        std::memcpy(&zip64EocdrOff, tail.constData() + eocdIdx - 12, 8);

        f.seek(zip64EocdrOff);
        char zip64Buf[56]; f.read(zip64Buf, 56);
        uint32_t z64Sig;
        std::memcpy(&z64Sig, zip64Buf, 4);
        if (z64Sig != 0x06064b50) {
            qWarning() << "[ZIP] Invalid ZIP64 EOCDR signature";
            return {};
        }
        uint32_t zip64DiskNum;
        std::memcpy(&zip64DiskNum, zip64Buf + 16, 4);
        if (zip64DiskNum != 0) {
            qWarning() << "[ZIP] Multi-disk ZIP64 not supported";
            return {};
        }
        uint64_t zip64Entries, zip64CdSize, zip64CdOff;
        std::memcpy(&zip64Entries, zip64Buf + 32, 8);
        std::memcpy(&zip64CdSize,  zip64Buf + 40, 8);
        std::memcpy(&zip64CdOff,   zip64Buf + 48, 8);

        if (cdCount == 0xFFFF) cdCount = static_cast<uint16_t>(
            zip64Entries < 0xFFFF ? zip64Entries : 0xFFFF);
        if (cdSize  == 0xFFFFFFFF) cdSize = static_cast<uint32_t>(
            zip64CdSize < 0xFFFFFFFFULL ? zip64CdSize : 0xFFFFFFFF);
        cdOff = zip64CdOff;

        qDebug() << "[ZIP] ZIP64: entries=" << zip64Entries
                 << "cdSize=" << zip64CdSize << "cdOff=" << zip64CdOff;
    }

    if (cdSize == 0 || cdOff == 0 || cdOff + cdSize > static_cast<uint64_t>(fsize)) {
        qWarning() << "[ZIP] Invalid CD bounds cdOff=" << cdOff << "cdSize=" << cdSize;
        return {};
    }

    // 扫描 central directory
    f.seek(cdOff);
    QByteArray cdBuf = f.read(cdSize);
    const char* p = cdBuf.constData();
    const char* end = p + cdBuf.size();

    uint32_t lhOff = 0, compSize = 0, uncompSize = 0;
    uint16_t method = 0;
    QByteArray entryUtf8 = entryName.toUtf8();

    for (uint16_t i = 0; i < cdCount && p + 46 <= end; ++i) {
        uint32_t sig;  std::memcpy(&sig, p, 4);
        if (sig != kCdEntrySig) break;

        uint16_t fnLen, extraLen, commentLen;
        std::memcpy(&fnLen,      p + 28, 2);
        std::memcpy(&extraLen,   p + 30, 2);
        std::memcpy(&commentLen, p + 32, 2);

        QByteArray fn(p + 46, fnLen);
        if (fn == entryUtf8) {
            std::memcpy(&method,     p + 10, 2);
            std::memcpy(&compSize,   p + 20, 4);
            std::memcpy(&uncompSize, p + 24, 4);
            std::memcpy(&lhOff,      p + 42, 4);
            break;
        }
        p += 46 + fnLen + extraLen + commentLen;
    }
    if (!lhOff || !uncompSize) {
        qWarning() << "[ZIP] Entry not found:" << entryName;
        return {};
    }

    // 读取 local header → 跳过 filename + extra → 定位压缩数据
    f.seek(lhOff);
    char lh[30]; f.read(lh, 30);
    uint16_t lFnLen, lExtraLen;
    std::memcpy(&lFnLen,    lh + 26, 2);
    std::memcpy(&lExtraLen, lh + 28, 2);
    qint64 dataOff = lhOff + 30 + lFnLen + lExtraLen;

    f.seek(dataOff);
    uint32_t readSize = compSize > 0 ? compSize
                       : static_cast<uint32_t>(fsize - dataOff);
    QByteArray compData = f.read(readSize);

    f.close();

    std::vector<unsigned char> result(uncompSize);

    if (method == kMethodDeflate && compSize > 0) {
        if (!loadZlibOnce() || !p_InflateInit2_ || !p_Inflate || !p_InflateEnd) {
            qWarning() << "[ZIP] zlib not available";
            return {};
        }

        ZStream strm;
        if (p_InflateInit2_(&strm, -MAX_WBITS, "1.2.11", sizeof(ZStream)) != Z_OK)
            return {};

        strm.next_in  = reinterpret_cast<const uint8_t*>(compData.constData());
        strm.avail_in = static_cast<uint32_t>(compData.size());
        strm.next_out  = result.data();
        strm.avail_out = static_cast<uint32_t>(uncompSize);

        int ret = p_Inflate(&strm, Z_FINISH);
        p_InflateEnd(&strm);

        if (ret != Z_STREAM_END) {
            qWarning() << "[ZIP] inflate failed, ret=" << ret;
            return {};
        }
    } else if (method == 0 && compSize == 0) {
        std::memcpy(result.data(), compData.constData(), uncompSize);
    } else {
        qWarning() << "[ZIP] Unsupported method" << method << "or stored size mismatch";
        return {};
    }

    return result;
}

// ── 最小 TIFF IFD 解析: 提取 StripOffsets / StripByteCounts 首值 ──
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

    auto tagVal = [&](const uint8_t* tp, uint32_t* countOut) -> uint32_t {
        uint16_t type  = r16(tp + 2);
        uint32_t count = r32(tp + 4);
        uint32_t val   = r32(tp + 8);
        if (countOut) *countOut = count;
        uint32_t typeSize = (type == 3) ? 2 : (type == 4) ? 4 : 1;
        if (count * typeSize <= 4)
            return val;
        else if (val + count * typeSize <= static_cast<uint32_t>(dataSize))
            return r32(data + val);
        return 0;
    };

    uint32_t stripOff = 0, stripBC = 0;
    for (int i = 0; i < numTags; ++i, tp += 12) {
        if (tp + 12 > data + dataSize) return false;
        uint16_t tag = r16(tp);
        if (tag == 273) stripOff = tagVal(tp, nullptr);
        else if (tag == 279) stripBC = tagVal(tp, nullptr);
        if (stripOff && stripBC) break;
    }
    if (!stripOff || !stripBC) return false;

    *outFirstStripOff = stripOff;
    *outStripByteCount = stripBC;
    return true;
}

// ═══════════════════════════════════════════════
//  SentinelDataReader
// ═══════════════════════════════════════════════

SentinelDataReader::~SentinelDataReader() { close(); }

bool SentinelDataReader::open(const QString& zipPath, const QString& entryName,
                               const SarBandDescriptor& band)
{
    close();

    // ═══ Step 1: ZIP 解压完整的 TIFF ═══
    mRawTiff = readZipEntry(zipPath, entryName);
    if (mRawTiff.empty()) {
        qWarning() << "[SDR] readZipEntry failed:" << zipPath << entryName;
        return false;
    }

    // ═══ Step 2: 解析 TIFF IFD → 获取 strip 偏移 ═══
    uint32_t firstStripOff = 0, stripByteCount = 0;
    if (!parseTiffStripOffset(mRawTiff.data(), mRawTiff.size(),
            &firstStripOff, &stripByteCount)) {
        qWarning() << "[SDR] TIFF IFD parse failed";
        close();
        return false;
    }

    // ═══ Step 3: /vsimem/ → GDAL 读宽/高/地理参考 ═══
    mMemPath = QString("/vsimem/_sdr_%1.tif").arg(
        reinterpret_cast<quintptr>(this), 0, 16);
    VSIFCloseL(VSIFileFromMemBuffer(mMemPath.toUtf8().constData(),
        mRawTiff.data(), mRawTiff.size(), FALSE));

    mVsiDataset = GDALOpenShared(mMemPath.toUtf8().constData(), GA_ReadOnly);
    if (!mVsiDataset) {
        qWarning() << "[SDR] GDALOpenShared /vsimem/ failed";
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

    // ═══ Step 4: 直接 strip → CInt16→SoA, 切分 burst ═══
    mCaches.resize(mBurstCount);
    for (int b = 0; b < mBurstCount; ++b) {
        int row0 = b * mLinesPerBurst;
        int burstH = mLinesPerBurst;
        if (b == mBurstCount - 1)
            burstH = mHeight - row0;
        if (burstH <= 0 || row0 < 0 || row0 + burstH > mHeight) {
            qWarning() << "[SDR] Invalid burst bounds" << b; continue;
        }

        size_t burstOff = static_cast<size_t>(firstStripOff)
                        + static_cast<size_t>(row0) * stripByteCount;
        if (burstOff + static_cast<size_t>(burstH) * stripByteCount > mRawTiff.size()) {
            qWarning() << "[SDR] Strip OOB burst" << b; continue;
        }

        mCaches[b].loadFromRawStrips(mRawTiff.data() + burstOff, mWidth, burstH);
        // deramp 由 pipeline step (TOPSARDeramp) 显式调用, 不在此处自动执行
    }

    qDebug() << "[SDR] Loaded" << mBurstCount << "bursts" << mWidth << "x" << mHeight;
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

void SentinelDataReader::derampBurst(int burstIdx, double prf, double kt)
{
    if (burstIdx < 0 || burstIdx >= static_cast<int>(mCaches.size())) return;
    if (!mCaches[burstIdx].isLoaded()) return;
    int row0 = burstIdx * mLinesPerBurst;
    mCaches[burstIdx].applyDeramp(prf, kt, row0, burstIdx);
}

bool SentinelDataReader::readWindow(int x0, int y0, int w, int h, std::complex<float>* dst)
{
    if (!dst || w <= 0 || h <= 0) return false;
    if (x0 < 0 || y0 < 0 || x0 + w > mWidth || y0 + h > mHeight) return false;

    // TOPSAR 路径: 使用已加载的 burst cache (SoA 数据已缓存)
    if (mBurstCount > 0 && mLinesPerBurst > 0) {
        std::fill(dst, dst + static_cast<size_t>(w) * h, std::complex<float>(0.0f, 0.0f));

        int burstFirst = y0 / mLinesPerBurst;
        int burstLast  = (y0 + h - 1) / mLinesPerBurst;
        if (burstFirst < 0) burstFirst = 0;
        if (burstLast >= mBurstCount) burstLast = mBurstCount - 1;

        for (int bi = burstFirst; bi <= burstLast; ++bi) {
            if (!mCaches[bi].isLoaded()) continue;
            int burstRow0 = bi * mLinesPerBurst;
            int burstH    = (bi == mBurstCount - 1) ? mHeight - burstRow0 : mLinesPerBurst;

            int interY0 = std::max(y0, burstRow0);
            int interY1 = std::min(y0 + h, burstRow0 + burstH);
            if (interY0 >= interY1) continue;

            mCaches[bi].getWindow(x0, interY0 - burstRow0, w, interY1 - interY0,
                                  dst + (interY0 - y0) * w);
        }
        return true;
    }

    // 非TOPSAR 回退: 直接通过 GDAL 读取 (如 IfgGenerator dummyBand 路径)
    if (!mVsiDataset) return false;
    CPLErr err = GDALRasterIO(
        GDALGetRasterBand(static_cast<GDALDatasetH>(mVsiDataset), 1),
        GF_Read, x0, y0, w, h,
        dst, w, h, GDT_CFloat32, 0, 0);
    return err == CE_None;
}
