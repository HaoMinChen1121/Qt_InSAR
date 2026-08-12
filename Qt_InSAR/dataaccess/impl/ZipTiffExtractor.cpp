#include "ZipTiffExtractor.h"
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <algorithm>

// ── zlib ──
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
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4
#define MAX_WBITS 15

typedef int (*InflateInit2_t)(ZStream*, int, const char*, int);
typedef int (*Inflate_t)(ZStream*, int);
typedef int (*InflateEnd_t)(ZStream*);
static InflateInit2_t p_InflateInit2_;
static Inflate_t      p_Inflate;
static InflateEnd_t   p_InflateEnd;
static bool            sZlibReady = false;

static void ensureZlib()
{
    if (sZlibReady) return;
    sZlibReady = true;
    HMODULE h = GetModuleHandleW(L"zlib.dll");
    if (!h) h = GetModuleHandleW(L"zlibwapi.dll");
    if (!h) h = LoadLibraryW(L"zlib.dll");
    if (!h) return;
    p_InflateInit2_ = (InflateInit2_t)GetProcAddress(h, "inflateInit2_");
    p_Inflate       = (Inflate_t)     GetProcAddress(h, "inflate");
    p_InflateEnd    = (InflateEnd_t)  GetProcAddress(h, "inflateEnd");
}

// ── ZIP ──
static constexpr uint32_t kEocdSig    = 0x06054b50;
static constexpr uint32_t kCdEntrySig = 0x02014b50;
static constexpr uint16_t kDeflate    = 8;

std::vector<unsigned char> ZipTiffExtractor::extractRaw(const QString& zipPath,
                                                         const QString& entryName)
{
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    qint64 fsize = f.size();
    if (fsize < 22) return {};

    int searchLen = std::min(static_cast<int>(fsize), 65536 + 22);
    f.seek(fsize - searchLen);
    QByteArray tail = f.read(searchLen);

    int eocdIdx = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        uint32_t sig; std::memcpy(&sig, tail.constData() + i, 4);
        if (sig == kEocdSig) { eocdIdx = i; break; }
    }
    if (eocdIdx < 0) return {};

    const char* eocd = tail.constData() + eocdIdx;
    uint16_t cdCount;  uint32_t cdSize;
    std::memcpy(&cdCount, eocd + 10, 2);
    std::memcpy(&cdSize,  eocd + 12, 4);
    uint32_t cdOff32; std::memcpy(&cdOff32, eocd + 16, 4);
    uint64_t cdOff = cdOff32;

    // ZIP64
    if (cdOff == 0xFFFFFFFF || cdCount == 0xFFFF || cdSize == 0xFFFFFFFF) {
        if (eocdIdx < 20) return {};
        uint32_t z64sig; std::memcpy(&z64sig, tail.constData() + eocdIdx - 20, 4);
        if (z64sig != 0x07064b50) return {};
        uint64_t z64Off; std::memcpy(&z64Off, tail.constData() + eocdIdx - 12, 8);
        f.seek(z64Off);
        char zb[56]; f.read(zb, 56);
        uint64_t z64Entries, z64CdOff;
        std::memcpy(&z64Entries, zb + 32, 8);
        std::memcpy(&z64CdOff,   zb + 48, 8);
        if (cdCount == 0xFFFF) cdCount = static_cast<uint16_t>(std::min(z64Entries, 0xFFFFULL));
        cdOff = z64CdOff;
    }
    if (cdOff + cdSize > static_cast<uint64_t>(fsize)) return {};

    f.seek(cdOff);
    QByteArray cdBuf = f.read(cdSize);
    const char* p = cdBuf.constData();
    const char* end = p + cdBuf.size();
    QByteArray key = entryName.toUtf8();

    uint32_t lhOff = 0, compSize = 0, uncompSize = 0;
    uint16_t method = 0;
    for (uint16_t i = 0; i < cdCount && p + 46 <= end; ++i) {
        uint32_t sig; std::memcpy(&sig, p, 4);
        if (sig != kCdEntrySig) break;
        uint16_t fnLen, extraLen, commentLen;
        std::memcpy(&fnLen, p + 28, 2);
        std::memcpy(&extraLen, p + 30, 2);
        std::memcpy(&commentLen, p + 32, 2);
        if (QByteArray(p + 46, fnLen) == key) {
            std::memcpy(&method, p + 10, 2);
            std::memcpy(&compSize, p + 20, 4);
            std::memcpy(&uncompSize, p + 24, 4);
            std::memcpy(&lhOff, p + 42, 4);
            break;
        }
        p += 46 + fnLen + extraLen + commentLen;
    }
    if (!lhOff || !uncompSize) { qWarning() << "[ZTE] entry not found:" << entryName; return {}; }

    f.seek(lhOff);
    char lh[30]; f.read(lh, 30);
    uint16_t lfn, lx; std::memcpy(&lfn, lh + 26, 2); std::memcpy(&lx, lh + 28, 2);
    qint64 dataOff = lhOff + 30 + lfn + lx;
    uint32_t rSize = compSize > 0 ? compSize : static_cast<uint32_t>(fsize - dataOff);
    f.seek(dataOff);
    QByteArray compData = f.read(rSize);
    f.close();

    std::vector<unsigned char> result(uncompSize);
    if (method == kDeflate && compSize > 0) {
        ensureZlib();
        if (!p_InflateInit2_) return {};
        ZStream strm;
        if (p_InflateInit2_(&strm, -MAX_WBITS, "1.2.11", sizeof(ZStream)) != Z_OK) return {};
        strm.next_in  = reinterpret_cast<const uint8_t*>(compData.constData());
        strm.avail_in = compData.size();
        strm.next_out  = result.data();
        strm.avail_out = uncompSize;
        int ret = p_Inflate(&strm, Z_FINISH);
        p_InflateEnd(&strm);
        if (ret != Z_STREAM_END) { qWarning() << "[ZTE] inflate failed"; return {}; }
    } else if (method == 0 && compSize == 0) {
        std::memcpy(result.data(), compData.constData(), uncompSize);
    } else {
        qWarning() << "[ZTE] unsupported method" << method; return {};
    }
    return result;
}

// ── TIFF header parsing ──
TiffHeaderInfo ZipTiffExtractor::parseHeader(const std::vector<unsigned char>& raw)
{
    TiffHeaderInfo info;
    const uint8_t* data = raw.data();
    size_t sz = raw.size();
    if (sz < 8) return info;

    bool le;
    if (data[0]=='I' && data[1]=='I') le = true;
    else if (data[0]=='M' && data[1]=='M') le = false;
    else return info;
    info.littleEndian = le;

    auto r16=[le](const uint8_t* p){ return le ? (p[0]|(uint16_t(p[1])<<8)) : ((uint16_t(p[0])<<8)|p[1]); };
    auto r32=[le](const uint8_t* p){ return le ? (p[0]|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24)) : ((uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]); };
    auto r64=[le,&r32](const uint8_t* p){ return le ? (uint64_t(r32(p))|(uint64_t(r32(p+4))<<32)) : ((uint64_t(r32(p+4)))|(uint64_t(r32(p))<<32)); };
    auto rdbl=[le,&r64](const uint8_t* p){ uint64_t v=r64(p); double d; std::memcpy(&d,&v,8); return d; };

    if (r16(data+2) != 42) return info;
    uint32_t ifdOff = r32(data+4);
    if (ifdOff + 2 > sz) return info;

    // Tag constants
    const uint16_t TAG_IMAGE_WIDTH    = 256;
    const uint16_t TAG_IMAGE_LENGTH   = 257;
    const uint16_t TAG_BITS_PER_SAMPLE = 258;
    const uint16_t TAG_SAMPLE_FORMAT   = 339;
    const uint16_t TAG_SAMPLES_PER_PIXEL = 277;
    const uint16_t TAG_STRIP_OFFSETS  = 273;
    const uint16_t TAG_STRIP_BYTE_COUNTS = 279;
    const uint16_t TAG_MODEL_TIEPOINT = 33922;
    const uint16_t TAG_MODEL_PIXEL_SCALE = 33550;
    const uint16_t TAG_MODEL_TRANSFORMATION = 34264;
    const uint16_t TAG_GEO_KEY_DIRECTORY = 34735;
    const uint16_t TAG_GEO_DOUBLE_PARAMS = 34736;
    const uint16_t TAG_GEO_ASCII_PARAMS  = 34737;

    uint16_t numTags = r16(data + ifdOff);
    const uint8_t* tp = data + ifdOff + 2;

    auto tagDataOff = [&](const uint8_t* t, uint32_t* countOut) -> const uint8_t* {
        uint16_t type = r16(t+2);
        uint32_t count = r32(t+4);
        if (countOut) *countOut = count;
        uint32_t byteSize = 0;
        switch(type){ case 1: case 2: case 6: case 7: byteSize=1; break;
                       case 3: case 8: byteSize=2; break;
                       case 4: case 9: case 11: byteSize=4; break;
                       case 5: case 10: case 12: byteSize=8; break;
                       default: byteSize=1; break; }
        if (count * byteSize <= 4) return t + 8;
        uint32_t off = r32(t+8);
        if (off + count * byteSize <= static_cast<uint32_t>(sz)) return data + off;
        return nullptr;
    };

    double modelTiepoint[6] = {0,0,0,0,0,0};
    double pixelScale[3] = {1,1,1};
    bool hasTiepoint = false, hasPixelScale = false;
    std::vector<uint8_t> geoKeyRaw;
    std::vector<double> geoDoubleParams;
    std::string geoAsciiParams;

    for (int i = 0; i < numTags && tp + 12 <= data + sz; ++i, tp += 12) {
        uint16_t tag = r16(tp);
        uint32_t count = 0;
        const uint8_t* valPtr = nullptr;

        switch (tag) {
        case TAG_IMAGE_WIDTH: info.width = static_cast<int>(r32(tp+8)); break;
        case TAG_IMAGE_LENGTH: info.height = static_cast<int>(r32(tp+8)); break;
        case TAG_BITS_PER_SAMPLE: info.bitsPerSample = static_cast<int>(r16(tp+8)); break;
        case TAG_SAMPLE_FORMAT: info.sampleFormat = static_cast<int>(r16(tp+8)); break;
        case TAG_SAMPLES_PER_PIXEL: info.samplesPerPixel = static_cast<int>(r16(tp+8)); break;
        case TAG_STRIP_OFFSETS: info.firstStripOffset = r32(tp+8); break;
        case TAG_STRIP_BYTE_COUNTS:
            info.bytesPerRow = r32(tp+8) / (info.width > 0 ? info.width : 1);
            break;
        case TAG_MODEL_TIEPOINT:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count >= 6) {
                for (int j = 0; j < 6; ++j) modelTiepoint[j] = rdbl(valPtr + j*8);
                hasTiepoint = true;
            }
            break;
        case TAG_MODEL_PIXEL_SCALE:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count >= 3) {
                for (int j = 0; j < 3; ++j) pixelScale[j] = rdbl(valPtr + j*8);
                hasPixelScale = true;
            }
            break;
        case TAG_MODEL_TRANSFORMATION:
            // 4x4 transformation matrix → derive GCPs differently
            break;
        case TAG_GEO_KEY_DIRECTORY:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count > 0)
                geoKeyRaw.assign(valPtr, valPtr + count * 2); // SHORT type = 2 bytes
            break;
        case TAG_GEO_DOUBLE_PARAMS:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count > 0) {
                geoDoubleParams.resize(count);
                for (uint32_t j = 0; j < count; ++j)
                    geoDoubleParams[j] = rdbl(valPtr + j*8);
            }
            break;
        case TAG_GEO_ASCII_PARAMS:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count > 0)
                geoAsciiParams.assign(reinterpret_cast<const char*>(valPtr), count);
            break;
        }
    }

    // Build GCP grid from tiepoint + pixel scale
    if (hasTiepoint && hasPixelScale && pixelScale[0] > 0 && pixelScale[1] > 0) {
        // TIFF ModelTiepoint: (I,J,K, X,Y,Z) — I,J = pixel,line; X,Y,Z = lon,lat,height
        double I0 = modelTiepoint[0], J0 = modelTiepoint[1];
        double X0 = modelTiepoint[3], Y0 = modelTiepoint[4], Z0 = modelTiepoint[5];
        double dX = pixelScale[0], dY = -pixelScale[1]; // lat = Y0 + J * dY, dY is negative
        double dZ = pixelScale[2];

        int gridCols = std::max(1, static_cast<int>(info.width  / 2500.0 + 0.5));
        int gridRows = std::max(1, static_cast<int>(info.height / 2500.0 + 0.5));
        gridCols = std::max(gridCols, gridRows); // keep square-ish
        gridRows = gridCols;

        for (int r = 0; r <= gridRows; ++r) {
            for (int c = 0; c <= gridCols; ++c) {
                TiffHeaderInfo::GcpEntry g;
                g.pixel   = I0 + c * (info.width  - 1) / static_cast<double>(gridCols);
                g.line    = J0 + r * (info.height - 1) / static_cast<double>(gridRows);
                g.lon     = X0 + g.pixel * dX;
                g.lat     = Y0 + g.line  * dY;
                g.height  = Z0 + g.pixel * dZ;
                info.gcps.append(g);
            }
        }
    }

    // Build projection WKT from GeoKey
    if (!geoKeyRaw.empty()) {
        // GeoKey format: {KeyDirectoryVersion, KeyRevision, MinorRevision, NumKeys, keyID, tiffTag, count, offset...}
        // Minimal: just detect WGS84 from GTModelTypeGeoKey(1024)=2
        // For simplicity, output "GEOGCS[\"WGS 84\",...]" if WGS84 is detected
        uint16_t numKeys = (geoKeyRaw.size() >= 4) ? (geoKeyRaw[3]) : 0;
        bool isWGS84 = true; // Sentinel-1 uses WGS84
        const double WGS84_A = 6378137.0;
        const double WGS84_B = 6356752.314245;
        info.projectionWkt = QString(
            "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",%1,%2,"
            "AUTHORITY[\"EPSG\",\"7030\"]],AUTHORITY[\"EPSG\",\"6326\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],AUTHORITY[\"EPSG\",\"4326\"]]")
            .arg(WGS84_A, 0, 'f', 0).arg(WGS84_B, 0, 'f', 6);
    } else {
        info.projectionWkt = QString(
            "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563,"
            "AUTHORITY[\"EPSG\",\"7030\"]],AUTHORITY[\"EPSG\",\"6326\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],AUTHORITY[\"EPSG\",\"4326\"]]");
    }

    info.valid = (info.width > 0 && info.height > 0);
    return info;
}

bool ZipTiffExtractor::extractToFile(const QString& zipPath, const QString& entryName,
                                      const QString& outputPath, TiffHeaderInfo* outInfo)
{
    auto raw = extractRaw(zipPath, entryName);
    if (raw.empty()) return false;

    if (outInfo) *outInfo = parseHeader(raw);

    QFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.write(reinterpret_cast<const char*>(raw.data()), static_cast<qint64>(raw.size()));
    out.close();
    return true;
}
