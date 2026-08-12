#ifndef ZIPTIFFEXTRACTOR_H
#define ZIPTIFFEXTRACTOR_H

#include <QString>
#include <QVector>
#include <vector>
#include <cstdint>

// ── TIFF header 解析结果 (零 GDAL 依赖) ──
struct TiffHeaderInfo {
    bool    valid = false;
    bool    littleEndian = true;
    int     width  = 0;
    int     height = 0;
    int     bitsPerSample = 16;
    int     sampleFormat = 0;    // 0=uint, 2=int, 3=float
    int     samplesPerPixel = 2; // CInt16=2
    int     rowsPerStrip = 1;
    uint32_t firstStripOffset = 0;
    uint32_t bytesPerRow = 0;    // 每行字节数 (rowsPerStrip>1 时按 strip 均摊)

    // 完整 strip 索引 (用于按行定位)
    QVector<uint64_t> stripOffsets;
    QVector<uint64_t> stripByteCounts;

    // 地理参考: ModelTiepointTag + ModelPixelScaleTag 原值
    double tiepoint[6] = {0, 0, 0, 0, 0, 0};
    double pixelScale[3] = {0, 0, 0};
    bool   hasTiepointPixelScale = false;

    // GCP 列表 (从 ModelTiepointTag + ModelPixelScaleTag 构建)
    struct GcpEntry {
        double pixel, line, lon, lat, height;
    };
    QVector<GcpEntry> gcps;
    QString projectionWkt;

    // 直接仿射 geotransform (含旋转项, 来自网格 LSQ 拟合)
    double geoTransform[6] = {0, 0, 0, 0, 0, 0};
    bool   hasGeoTransform = false;

    // 场景统计预设 (来自 annotation imageStatistics, 用于首帧拉伸)
    // 0 表示无预设, 使用固定回退值
    double presetMean = 0.0, presetStd = 0.0, presetMax = 0.0;

    // S1 单 tiepoint → 精确仿射 geotransform
    // GT = (X0 - I0*dX, dX, 0, Y0 - J0*dY, 0, dY), dY = -pixelScale[1]
    bool computeGeotransform(double gt[6]) const
    {
        if (!hasTiepointPixelScale || pixelScale[0] <= 0 || pixelScale[1] <= 0)
            return false;
        gt[0] = tiepoint[3] - tiepoint[0] * pixelScale[0];
        gt[1] = pixelScale[0];
        gt[2] = 0.0;
        gt[3] = tiepoint[4] - tiepoint[1] * (-pixelScale[1]);
        gt[4] = 0.0;
        gt[5] = -pixelScale[1];
        return true;
    }

    // 优先直接仿射, 回退 tiepoint/pixelScale
    bool getGeoTransform(double gt[6]) const
    {
        if (hasGeoTransform) {
            for (int i = 0; i < 6; ++i) gt[i] = geoTransform[i];
            return true;
        }
        return computeGeotransform(gt);
    }

    // SampleFormat: 3=float, 5=ComplexInt, 6=ComplexFloat
    bool isComplex() const
    {
        return samplesPerPixel >= 2 || sampleFormat == 3
            || sampleFormat == 5 || sampleFormat == 6;
    }
};

/// 从 ZIP 中提取 TIFF 条目并解析头信息 (零 GDAL 依赖)
class ZipTiffExtractor {
public:
    /// 从 ZIP 提取原始 TIFF 字节 (整读, 仅用于小文件/处理流水线)
    static std::vector<unsigned char> extractRaw(const QString& zipPath,
                                                  const QString& entryName);

    /// 只 inflate TIFF 头部 (IFD + strip 数组), 不读像素数据
    /// 自动加大预算直到 strip 数组完整 (256KB → 4MB)
    static TiffHeaderInfo extractHeader(const QString& zipPath,
                                        const QString& entryName);

    /// 解析 TIFF header (宽/高/rowsPerStrip/strip数组/GCP)
    static TiffHeaderInfo parseHeader(const std::vector<unsigned char>& rawTiff);

    /// 网格点 (pixel,line→lon,lat) 最小二乘拟合 6 参数仿射 geotransform
    static bool fitGeoTransform(const QVector<TiffHeaderInfo::GcpEntry>& points,
                                double gt[6]);

    /// WGS84 WKT (S1 产品标准)
    static QString wgs84Wkt()
    {
        return QStringLiteral(
            "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563,"
            "AUTHORITY[\"EPSG\",\"7030\"]],AUTHORITY[\"EPSG\",\"6326\"]],"
            "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
            "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
            "AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST],AUTHORITY[\"EPSG\",\"4326\"]]");
    }

    /// 一步完成: 提取+解析+写入本地文件
    static bool extractToFile(const QString& zipPath, const QString& entryName,
                              const QString& outputPath, TiffHeaderInfo* outInfo = nullptr);

private:
    ZipTiffExtractor() = delete;
};

#endif // ZIPTIFFEXTRACTOR_H
