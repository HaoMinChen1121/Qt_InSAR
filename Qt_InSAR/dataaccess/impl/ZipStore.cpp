#include "ZipStore.h"
#include <QFileInfo>
#include <QDebug>
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// ═══════════════════════════════════════════════
//  zlib 动态加载 (进程级共享)
// ═══════════════════════════════════════════════

#define Z_OK          0
#define Z_STREAM_END  1
#define Z_NO_FLUSH    0
#define Z_FINISH      4
#define MAX_WBITS     15

typedef int (*InflateInit2_t)(ZlibStream*, int, const char*, int);
typedef int (*Inflate_t)(ZlibStream*, int);
typedef int (*InflateEnd_t)(ZlibStream*);
typedef int (*InflateReset_t)(ZlibStream*);

namespace {
InflateInit2_t  p_InflateInit2_ = nullptr;
Inflate_t       p_Inflate       = nullptr;
InflateEnd_t    p_InflateEnd    = nullptr;
InflateReset_t  p_InflateReset  = nullptr;
bool            sZlibLoaded = false;
bool            sZlibTried  = false;
}

namespace zlibshared {

bool ensureLoaded()
{
    if (sZlibTried) return sZlibLoaded;
    sZlibTried = true;

    // GDAL 通常已将 zlib 加载进进程空间
    HMODULE h = GetModuleHandleW(L"zlib.dll");
    if (!h) h = GetModuleHandleW(L"zlibwapi.dll");
    if (!h) h = LoadLibraryW(L"zlib.dll");
    if (!h) {
        qWarning() << "[ZipStore] Cannot load zlib.dll";
        return false;
    }
    p_InflateInit2_ = (InflateInit2_t)GetProcAddress(h, "inflateInit2_");
    p_Inflate       = (Inflate_t)     GetProcAddress(h, "inflate");
    p_InflateEnd    = (InflateEnd_t)  GetProcAddress(h, "inflateEnd");
    p_InflateReset  = (InflateReset_t)GetProcAddress(h, "inflateReset");
    sZlibLoaded = p_InflateInit2_ && p_Inflate && p_InflateEnd && p_InflateReset;
    if (!sZlibLoaded)
        qWarning() << "[ZipStore] zlib functions missing";
    return sZlibLoaded;
}

int inflateInit2(ZlibStream* s, int windowBits, const char* version, int streamSize)
{ return p_InflateInit2_(s, windowBits, version, streamSize); }
int inflate(ZlibStream* s, int flush) { return p_Inflate(s, flush); }
int inflateEnd(ZlibStream* s) { return p_InflateEnd(s); }
int inflateReset(ZlibStream* s) { return p_InflateReset(s); }

} // namespace zlibshared

// ═══════════════════════════════════════════════
//  ZipStore
// ═══════════════════════════════════════════════

namespace {
constexpr uint32_t kEocdSig    = 0x06054b50;
constexpr uint32_t kCdEntrySig = 0x02014b50;

QMutex  sStoreMutex;
QMap<QString, std::weak_ptr<ZipStore>> sStores;
}

std::shared_ptr<ZipStore> ZipStore::open(const QString& zipPath)
{
    QString key = QFileInfo(zipPath).absoluteFilePath().toLower();
    QMutexLocker lock(&sStoreMutex);
    auto it = sStores.find(key);
    if (it != sStores.end()) {
        if (auto s = it->lock()) return s;
        sStores.erase(it);
    }
    std::shared_ptr<ZipStore> store(new ZipStore);
    if (!store->load(zipPath)) return nullptr;
    sStores.insert(key, store);
    return store;
}

bool ZipStore::load(const QString& zipPath)
{
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[ZipStore] Cannot open:" << zipPath;
        return false;
    }
    qint64 fsize = f.size();
    if (fsize < 22) return false;

    int searchLen = std::min(static_cast<int>(fsize), 65536 + 22);
    f.seek(fsize - searchLen);
    QByteArray tail = f.read(searchLen);

    int eocdIdx = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        uint32_t sig;
        std::memcpy(&sig, tail.constData() + i, 4);
        if (sig == kEocdSig) { eocdIdx = i; break; }
    }
    if (eocdIdx < 0) { qWarning() << "[ZipStore] EOCD not found"; return false; }

    const char* eocd = tail.constData() + eocdIdx;
    uint16_t cdCount;
    uint32_t cdSize;
    std::memcpy(&cdCount,  eocd + 10, 2);
    std::memcpy(&cdSize,   eocd + 12, 4);
    uint32_t cdOff32;
    std::memcpy(&cdOff32,  eocd + 16, 4);
    uint64_t cdOff = cdOff32;

    // ZIP64
    if (cdOff == 0xFFFFFFFF || cdCount == 0xFFFF || cdSize == 0xFFFFFFFF) {
        if (eocdIdx < 20) { qWarning() << "[ZipStore] EOCD too close for ZIP64"; return false; }
        uint32_t zip64Sig;
        std::memcpy(&zip64Sig, tail.constData() + eocdIdx - 20, 4);
        if (zip64Sig != 0x07064b50) { qWarning() << "[ZipStore] ZIP64 locator not found"; return false; }
        uint64_t zip64EocdrOff;
        std::memcpy(&zip64EocdrOff, tail.constData() + eocdIdx - 12, 8);

        f.seek(zip64EocdrOff);
        char zip64Buf[56]; f.read(zip64Buf, 56);
        uint32_t z64Sig;
        std::memcpy(&z64Sig, zip64Buf, 4);
        if (z64Sig != 0x06064b50) { qWarning() << "[ZipStore] Invalid ZIP64 EOCDR signature"; return false; }
        uint32_t zip64DiskNum;
        std::memcpy(&zip64DiskNum, zip64Buf + 16, 4);
        if (zip64DiskNum != 0) { qWarning() << "[ZipStore] Multi-disk ZIP64 not supported"; return false; }
        uint64_t zip64Entries, zip64CdSize, zip64CdOff;
        std::memcpy(&zip64Entries, zip64Buf + 32, 8);
        std::memcpy(&zip64CdSize,  zip64Buf + 40, 8);
        std::memcpy(&zip64CdOff,   zip64Buf + 48, 8);
        if (cdCount == 0xFFFF) cdCount = static_cast<uint16_t>(
            zip64Entries < 0xFFFF ? zip64Entries : 0xFFFF);
        if (cdSize == 0xFFFFFFFF) cdSize = static_cast<uint32_t>(
            zip64CdSize < 0xFFFFFFFFULL ? zip64CdSize : 0xFFFFFFFF);
        cdOff = zip64CdOff;
    }

    if (cdSize == 0 || cdOff == 0 || cdOff + cdSize > static_cast<uint64_t>(fsize)) {
        qWarning() << "[ZipStore] Invalid CD bounds";
        return false;
    }

    f.seek(cdOff);
    QByteArray cdBuf = f.read(cdSize);
    const char* p = cdBuf.constData();
    const char* end = p + cdBuf.size();

    for (uint16_t i = 0; i < cdCount && p + 46 <= end; ++i) {
        uint32_t sig;
        std::memcpy(&sig, p, 4);
        if (sig != kCdEntrySig) break;

        uint16_t method, fnLen, extraLen, commentLen;
        uint32_t compSize, uncompSize, lhOff;
        std::memcpy(&method,    p + 10, 2);
        std::memcpy(&compSize,  p + 20, 4);
        std::memcpy(&uncompSize,p + 24, 4);
        std::memcpy(&fnLen,     p + 28, 2);
        std::memcpy(&extraLen,  p + 30, 2);
        std::memcpy(&commentLen,p + 32, 2);
        std::memcpy(&lhOff,     p + 42, 4);

        ZipEntryInfo e;
        e.name = QString::fromUtf8(QByteArray(p + 46, fnLen));
        e.method = method;
        e.compressedSize = compSize;
        e.uncompressedSize = uncompSize;

        // ZIP64 extra field (0x0001): 补全哨兵值
        // 布局 (按需出现): uncompressedSize(8) compressedSize(8) localHeaderOffset(8) diskNumber(4)
        uint64_t lhOff64 = lhOff;
        if (compSize == 0xFFFFFFFF || uncompSize == 0xFFFFFFFF || lhOff == 0xFFFFFFFF) {
            const char* xp = p + 46 + fnLen;
            const char* xe = xp + extraLen;
            while (xp + 4 <= xe) {
                uint16_t hdrId, hdrSize;
                std::memcpy(&hdrId,   xp, 2);
                std::memcpy(&hdrSize, xp + 2, 2);
                if (hdrId == 0x0001 && xp + 4 + hdrSize <= xe) {
                    const char* zp = xp + 4;
                    if (uncompSize == 0xFFFFFFFF && zp + 8 <= xe) {
                        std::memcpy(&e.uncompressedSize, zp, 8); zp += 8;
                    }
                    if (compSize == 0xFFFFFFFF && zp + 8 <= xe) {
                        std::memcpy(&e.compressedSize, zp, 8); zp += 8;
                    }
                    if (lhOff == 0xFFFFFFFF && zp + 8 <= xe) {
                        std::memcpy(&lhOff64, zp, 8);
                    }
                    break;
                }
                xp += 4 + hdrSize;
            }
        }

        // local header → 压缩数据起始
        char lh[30];
        if (f.seek(static_cast<qint64>(lhOff64)) && f.read(lh, 30) == 30) {
            uint16_t lFnLen, lExtraLen;
            std::memcpy(&lFnLen,    lh + 26, 2);
            std::memcpy(&lExtraLen, lh + 28, 2);
            e.dataOffset = lhOff64 + 30 + lFnLen + lExtraLen;
        } else {
            e.dataOffset = 0;
        }

        mEntries.insert(e.name.toLower(), e);
        p += 46 + fnLen + extraLen + commentLen;
    }

    mZipPath = zipPath;
    qDebug() << "[ZipStore] loaded" << mEntries.size() << "entries from" << zipPath;
    return !mEntries.isEmpty();
}

const ZipEntryInfo* ZipStore::findEntry(const QString& name) const
{
    auto it = mEntries.find(name.toLower());
    return it != mEntries.end() ? &it.value() : nullptr;
}

QStringList ZipStore::entryList() const
{
    QStringList out;
    for (auto it = mEntries.constBegin(); it != mEntries.constEnd(); ++it)
        out.append(it->name);
    return out;
}

std::vector<unsigned char> ZipStore::readEntry(const QString& entryName)
{
    const ZipEntryInfo* e = findEntry(entryName);
    if (!e) { qWarning() << "[ZipStore] entry not found:" << entryName; return {}; }
    return readEntry(*e);
}

std::vector<unsigned char> ZipStore::readEntry(const ZipEntryInfo& e)
{
    if (e.method == 0) {
        // stored: 直接读原始字节
        QFile f(mZipPath);
        if (!f.open(QIODevice::ReadOnly)) return {};
        if (!f.seek(e.dataOffset)) return {};
        QByteArray raw = f.read(e.uncompressedSize);
        if (static_cast<uint64_t>(raw.size()) != e.uncompressedSize) return {};
        return std::vector<unsigned char>(raw.begin(), raw.end());
    }

    std::unique_ptr<ZipInflateStream> s(openInflateStream(e));
    if (!s) return {};
    std::vector<unsigned char> out(e.uncompressedSize);
    if (e.uncompressedSize == 0) return out;
    int got = s->produce(out.data(), e.uncompressedSize);
    if (got != static_cast<int>(e.uncompressedSize)) {
        qWarning() << "[ZipStore] inflate short read" << got << "/" << e.uncompressedSize;
        return {};
    }
    return out;
}

ZipInflateStream* ZipStore::openInflateStream(const QString& entryName)
{
    const ZipEntryInfo* e = findEntry(entryName);
    if (!e) { qWarning() << "[ZipStore] stream entry not found:" << entryName; return nullptr; }
    return openInflateStream(*e);
}

ZipInflateStream* ZipStore::openInflateStream(const ZipEntryInfo& e)
{
    return new ZipInflateStream(mZipPath, e);
}

// ═══════════════════════════════════════════════
//  ZipInflateStream
// ═══════════════════════════════════════════════

ZipInflateStream::ZipInflateStream(const QString& zipPath, const ZipEntryInfo& entry)
{
    mDataOffset = entry.dataOffset;
    mCompressedSize = entry.compressedSize;
    mRemaining = entry.compressedSize;
    mMethod = entry.method;
    mFile.setFileName(zipPath);
    mInBuf.resize(256 * 1024);
    mZStream = static_cast<ZlibStream*>(std::malloc(sizeof(ZlibStream)));
    std::memset(mZStream, 0, sizeof(ZlibStream));
}

ZipInflateStream::~ZipInflateStream()
{
    if (mStarted)
        zlibshared::inflateEnd(mZStream);
    std::free(mZStream);
}

bool ZipInflateStream::feedInput()
{
    if (mRemaining == 0) {
        // 压缩数据耗尽但流未结束 → 截断, 视为结束
        mAtEnd = true;
        return false;
    }
    uint64_t chunk = std::min<uint64_t>(mRemaining, mInBuf.size());
    if (!mFile.isOpen() && !mFile.open(QIODevice::ReadOnly))
        return false;
    qint64 got = mFile.read(reinterpret_cast<char*>(mInBuf.data()), chunk);
    if (got <= 0) { mAtEnd = true; return false; }
    mRemaining -= static_cast<uint64_t>(got);
    mZStream->next_in = mInBuf.data();
    mZStream->avail_in = static_cast<uint32_t>(got);
    return true;
}

bool ZipInflateStream::restart()
{
    if (mMethod == 0) {
        // stored entry: 回到起始即可
        if (!mFile.isOpen() && !mFile.open(QIODevice::ReadOnly))
            return false;
        if (!mFile.seek(mDataOffset))
            return false;
        mPos = 0;
        mAtEnd = false;
        return true;
    }

    if (mStarted) {
        zlibshared::inflateReset(mZStream);
    } else {
        if (!zlibshared::ensureLoaded()) {
            qWarning() << "[ZipInflateStream] zlib unavailable";
            return false;
        }
        int rc = zlibshared::inflateInit2(mZStream, -MAX_WBITS, "1.2.11", sizeof(ZlibStream));
        if (rc != Z_OK) {
            qWarning() << "[ZipInflateStream] inflateInit2 failed rc=" << rc;
            return false;
        }
        mStarted = true;
    }
    if (!mFile.isOpen() && !mFile.open(QIODevice::ReadOnly)) {
        qWarning() << "[ZipInflateStream] cannot open" << mFile.fileName();
        return false;
    }
    if (!mFile.seek(mDataOffset)) {
        qWarning() << "[ZipInflateStream] seek failed";
        return false;
    }
    mZStream->next_in = nullptr;
    mZStream->avail_in = 0;
    mRemaining = mCompressedSize;
    mAtEnd = false;
    return true;
}

int ZipInflateStream::produce(uint8_t* dst, size_t wantBytes)
{
    if (mAtEnd || wantBytes == 0) return 0;

    if (mMethod == 0) {
        // stored entry: 直接读文件
        if (!mFile.isOpen() && !mFile.open(QIODevice::ReadOnly))
            return -1;
        if (!mFile.seek(mDataOffset + mPos))
            return -1;
        uint64_t left = mCompressedSize > mPos ? mCompressedSize - mPos : 0;
        size_t n = static_cast<size_t>(std::min<uint64_t>(wantBytes, left));
        qint64 got = mFile.read(reinterpret_cast<char*>(dst), n);
        if (got < 0) return -1;
        mPos += static_cast<uint64_t>(got);
        if (mPos >= mCompressedSize) mAtEnd = true;
        return static_cast<int>(got);
    }

    if (!mStarted) {
        if (!zlibshared::ensureLoaded()) {
            qWarning() << "[ZipInflateStream] zlib unavailable";
            return -1;
        }
        int rc = zlibshared::inflateInit2(mZStream, -MAX_WBITS, "1.2.11", sizeof(ZlibStream));
        if (rc != Z_OK) {
            qWarning() << "[ZipInflateStream] inflateInit2 failed rc=" << rc;
            return -1;
        }
        mStarted = true;
        if (!mFile.open(QIODevice::ReadOnly)) return -1;
        if (!mFile.seek(mDataOffset)) return -1;
    }

    mZStream->next_out = dst;
    mZStream->avail_out = static_cast<uint32_t>(wantBytes);

    while (mZStream->avail_out > 0) {
        if (mZStream->avail_in == 0) {
            if (!feedInput()) break;
        }
        int ret = zlibshared::inflate(mZStream, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) { mAtEnd = true; break; }
        if (ret != Z_OK) {
            qWarning() << "[ZipInflateStream] inflate error" << ret;
            return -1;
        }
    }
    return static_cast<int>(wantBytes - mZStream->avail_out);
}
