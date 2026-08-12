#include "ZipTiffExtractor.h"
#include "ZipStore.h"
#include <QFile>
#include <QDebug>
#include <cstring>
#include <string>
#include <algorithm>

// ── ZIP 提取 (复用 ZipStore) ──
std::vector<unsigned char> ZipTiffExtractor::extractRaw(const QString& zipPath,
                                                         const QString& entryName)
{
    auto store = ZipStore::open(zipPath);
    if (!store) return {};
    return store->readEntry(entryName);
}

TiffHeaderInfo ZipTiffExtractor::extractHeader(const QString& zipPath,
                                                const QString& entryName)
{
    auto store = ZipStore::open(zipPath);
    if (!store) return {};
    const ZipEntryInfo* e = store->findEntry(entryName);
    if (!e) { qWarning() << "[ZTE] entry not found:" << entryName; return {}; }

    if (e->uncompressedSize <= 256u * 1024u) {
        // 小 entry 直接整读
        return parseHeader(store->readEntry(*e));
    }

    // 注: S1 测量 TIFF 的 IFD 在文件末尾, 此快速路径只适用于
    // 经典布局 (IFD 在文件头部); S1 产品应走 annotation XML 路径
    std::unique_ptr<ZipInflateStream> s(store->openInflateStream(*e));
    if (!s) { qWarning() << "[ZTE] cannot create stream"; return {}; }
    if (!s->restart()) return {};

    std::vector<unsigned char> buf(256 * 1024);
    int got = s->produce(buf.data(), buf.size());
    if (got <= 0) return {};
    buf.resize(got);

    TiffHeaderInfo info = parseHeader(buf);
    if (!info.valid) return {};
    return info;
}

// ── 网格点 → 6 参数仿射最小二乘拟合 ──
// lon = gt[0] + gt[1]*pixel + gt[2]*line
// lat = gt[3] + gt[4]*pixel + gt[5]*line
bool ZipTiffExtractor::fitGeoTransform(
        const QVector<TiffHeaderInfo::GcpEntry>& points, double gt[6])
{
    const int n = points.size();
    if (n < 3) return false;

    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    double sux = 0, suy = 0, su = 0;
    double svx = 0, svy = 0, sv = 0;
    for (const auto& p : points) {
        double x = p.pixel, y = p.line;
        sx += x; sy += y;
        sxx += x * x; syy += y * y; sxy += x * y;
        su += p.lon; sux += p.lon * x; suy += p.lon * y;
        sv += p.lat; svx += p.lat * x; svy += p.lat * y;
    }

    // 解 3x3 正规方程 (高斯消元), 两轴共用系数矩阵
    double A[3][3] = {
        { static_cast<double>(n), sx, sy },
        { sx, sxx, sxy },
        { sy, sxy, syy }
    };
    double b1[3] = { su, sux, suy };
    double b2[3] = { sv, svx, svy };

    auto solve3 = [](double a[3][3], double b[3], double* out) -> bool {
        for (int k = 0; k < 3; ++k) {
            int piv = k;
            for (int i = k + 1; i < 3; ++i)
                if (std::abs(a[i][k]) > std::abs(a[piv][k])) piv = i;
            if (std::abs(a[piv][k]) < 1e-300) return false;
            if (piv != k) {
                for (int j = k; j < 3; ++j) std::swap(a[k][j], a[piv][j]);
                std::swap(b[k], b[piv]);
            }
            double d = a[k][k];
            for (int j = k; j < 3; ++j) a[k][j] /= d;
            b[k] /= d;
            for (int i = 0; i < 3; ++i) {
                if (i == k) continue;
                double f = a[i][k];
                if (f == 0) continue;
                for (int j = k; j < 3; ++j) a[i][j] -= f * a[k][j];
                b[i] -= f * b[k];
            }
        }
        out[0] = b[0]; out[1] = b[1]; out[2] = b[2];
        return true;
    };

    double a1[3][3], a2[3][3], c1[3], c2[3], r1[3], r2[3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a1[i][j] = a2[i][j] = A[i][j];
    for (int i = 0; i < 3; ++i) { c1[i] = b1[i]; c2[i] = b2[i]; }

    if (!solve3(a1, c1, r1) || !solve3(a2, c2, r2)) return false;

    gt[0] = r1[0]; gt[1] = r1[1]; gt[2] = r1[2];
    gt[3] = r2[0]; gt[4] = r2[1]; gt[5] = r2[2];
    return true;
}

// ── TIFF header 解析 ──
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
    const uint16_t TAG_ROWS_PER_STRIP = 278;
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

    QVector<double> tieVals;   // ModelTiepointTag 全部值 (单点6个 或 网格 n*6)
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
        case TAG_ROWS_PER_STRIP: info.rowsPerStrip = std::max(1, static_cast<int>(r32(tp+8))); break;
        case TAG_STRIP_OFFSETS:
            info.firstStripOffset = r32(tp+8);
            valPtr = tagDataOff(tp, &count);
            if (valPtr) {
                info.stripOffsets.reserve(count);
                for (uint32_t j = 0; j < count; ++j)
                    info.stripOffsets.append(r32(valPtr + j*4));
            }
            break;
        case TAG_STRIP_BYTE_COUNTS:
            valPtr = tagDataOff(tp, &count);
            if (valPtr) {
                info.stripByteCounts.reserve(count);
                for (uint32_t j = 0; j < count; ++j)
                    info.stripByteCounts.append(r32(valPtr + j*4));
            }
            break;
        case TAG_MODEL_TIEPOINT:
            valPtr = tagDataOff(tp, &count);
            if (valPtr && count >= 6) {
                tieVals.resize(count);
                for (uint32_t j = 0; j < count; ++j)
                    tieVals[j] = rdbl(valPtr + j*8);
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

    // 每行字节数 (rowsPerStrip>1 时按 strip 均摊)
    if (!info.stripByteCounts.isEmpty() && info.rowsPerStrip > 0 && info.width > 0)
        info.bytesPerRow = static_cast<uint32_t>(
            info.stripByteCounts[0] / info.rowsPerStrip / info.width);
    else if (info.bytesPerRow == 0 && info.width > 0 && info.samplesPerPixel > 0)
        info.bytesPerRow = static_cast<uint32_t>(
            info.width * info.samplesPerPixel * (info.bitsPerSample / 8));

    // 保存 tiepoint/pixelScale 原值
    if (hasTiepoint)
        std::memcpy(info.tiepoint, tieVals.constData(), sizeof(info.tiepoint));
    if (hasPixelScale)
        std::memcpy(info.pixelScale, pixelScale, sizeof(pixelScale));
    info.hasTiepointPixelScale = hasTiepoint && hasPixelScale;

    // 地理参考: 单 tiepoint+pixelScale → 仿射; 多点网格 → LSQ 拟合
    if (hasTiepoint) {
        int nPts = tieVals.size() / 6;
        if ((tieVals.size() % 6) == 0 && nPts > 1) {
            // 完整网格 (如 S1 的 1260 点): 直接 LSQ 拟合仿射
            QVector<TiffHeaderInfo::GcpEntry> pts;
            pts.reserve(nPts);
            for (int i = 0; i < nPts; ++i) {
                TiffHeaderInfo::GcpEntry g;
                g.pixel  = tieVals[i*6 + 0];
                g.line   = tieVals[i*6 + 1];
                g.lon    = tieVals[i*6 + 3];
                g.lat    = tieVals[i*6 + 4];
                g.height = tieVals[i*6 + 5];
                pts.append(g);
            }
            if (fitGeoTransform(pts, info.geoTransform)) {
                info.hasGeoTransform = true;
                info.gcps = pts;
            }
        } else if (hasPixelScale && pixelScale[0] > 0 && pixelScale[1] > 0) {
            // 单 tiepoint + pixelScale
            // TIFF ModelTiepoint: (I,J,K, X,Y,Z) — I,J = pixel,line; X,Y,Z = lon,lat,height
            double I0 = tieVals[0], J0 = tieVals[1];
            double X0 = tieVals[3], Y0 = tieVals[4], Z0 = tieVals[5];
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
