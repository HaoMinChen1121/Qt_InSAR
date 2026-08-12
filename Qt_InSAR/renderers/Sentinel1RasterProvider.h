#ifndef SENTINEL1RASTERPROVIDER_H
#define SENTINEL1RASTERPROVIDER_H

#include <qgsrasterdataprovider.h>
#include <qgsprovidermetadata.h>
#include "dataaccess/impl/ZipTiffExtractor.h"
#include <QString>
#include <memory>

class SentinelZipProduct;
class TiffStreamDecoder;

// 自定义 Raster Provider: ZIP 内 Sentinel-1 TIFF 直接渲染 (SNAP 式)
// URI: sentinel1zip:<percent-encoded "zipPath|entry|mode">
//   mode: amplitude (默认, 复数→Float32 振幅) | passthrough (实数直通)
class Sentinel1RasterProvider : public QgsRasterDataProvider
{
public:
    Sentinel1RasterProvider(const QString& uri,
                            const QgsDataProvider::ProviderOptions& options,
                            Qgis::DataProviderReadFlags flags);
    ~Sentinel1RasterProvider() override;

    QgsRasterDataProvider* clone() const override;
    Qgis::RasterInterfaceCapabilities capabilities() const override;

    QgsRasterBlock* block(int bandNo, const QgsRectangle& extent,
                          int width, int height,
                          QgsRasterBlockFeedback* feedback = nullptr) override;

    int xSize() const override { return mTiff ? mTiff->width : 0; }
    int ySize() const override { return mTiff ? mTiff->height : 0; }
    int bandCount() const override { return 1; }
    Qgis::DataType dataType(int bandNo) const override;
    Qgis::DataType sourceDataType(int bandNo) const override;
    QgsRectangle extent() const override;
    QgsCoordinateReferenceSystem crs() const override;

    bool isValid() const override { return mDecoder != nullptr; }
    QString name() const override;
    QString description() const override;
    QString lastErrorTitle() override { return QString(); }
    QString lastError() override { return mLastError; }

    QgsRasterBandStats bandStatistics(int bandNo,
        Qgis::RasterBandStatistics stats,
        const QgsRectangle& extent = QgsRectangle(),
        int sampleSize = 0,
        QgsRasterBlockFeedback* feedback = nullptr) override;

    bool setNoDataValue(int bandNo, double noDataValue) override;

    int xBlockSize() const override { return 256; }
    int yBlockSize() const override { return 256; }

    // 内部解码器 (供异步统计等使用; 共享指针保证生命周期安全)
    std::shared_ptr<TiffStreamDecoder> bandDecoder() const;

    // URI 编解码
    static QString buildUri(const QString& zipPath, const QString& entry,
                            const QString& mode = QStringLiteral("amplitude"));
    static bool parseUri(const QString& uri, QString* zipPath,
                         QString* entry, QString* mode);

private:
    QString mZipPath;
    QString mEntry;
    QString mMode;
    QString mLastError;
    std::shared_ptr<SentinelZipProduct> mProduct;
    std::shared_ptr<const TiffHeaderInfo> mTiff;
    std::shared_ptr<TiffStreamDecoder> mDecoder;
    QgsRectangle mExtent;
    QgsCoordinateReferenceSystem mCrs;
};

class Sentinel1ProviderMetadata : public QgsProviderMetadata
{
public:
    Sentinel1ProviderMetadata();
    QgsDataProvider* createProvider(const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags) override;
};

#endif // SENTINEL1RASTERPROVIDER_H
