#ifndef ZIPTIFFEXTRACTOR_H
#define ZIPTIFFEXTRACTOR_H

#include <QString>
#include <QVector>
#include <vector>
#include <cstdint>

// ── 最小 TIFF header 解析结果 ──
struct TiffHeaderInfo {
    bool    valid = false;
    bool    littleEndian = true;
    int     width  = 0;
    int     height = 0;
    int     bitsPerSample = 16;
    int     sampleFormat = 0;    // 0=uint, 2=int, 3=float
    int     samplesPerPixel = 2; // CInt16=2
    uint32_t firstStripOffset = 0;
    uint32_t bytesPerRow = 0;

    // GCP 列表 (从 ModelTiepointTag + ModelPixelScaleTag 构建)
    struct GcpEntry {
        double pixel, line, lon, lat, height;
    };
    QVector<GcpEntry> gcps;
    QString projectionWkt;
};

/// 从 ZIP 中提取 TIFF 条目并解析头信息 (零 GDAL 依赖)
class ZipTiffExtractor {
public:
    /// 从 ZIP 提取原始 TIFF 字节
    static std::vector<unsigned char> extractRaw(const QString& zipPath,
                                                  const QString& entryName);

    /// 解析 TIFF header (宽/高/GCP)
    static TiffHeaderInfo parseHeader(const std::vector<unsigned char>& rawTiff);

    /// 一步完成: 提取+解析+写入本地文件
    static bool extractToFile(const QString& zipPath, const QString& entryName,
                              const QString& outputPath, TiffHeaderInfo* outInfo = nullptr);

private:
    ZipTiffExtractor() = delete;
};

#endif // ZIPTIFFEXTRACTOR_H
