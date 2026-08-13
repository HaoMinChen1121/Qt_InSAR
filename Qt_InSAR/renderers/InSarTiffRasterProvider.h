#ifndef INSARTIFFRASTERPROVIDER_H
#define INSARTIFFRASTERPROVIDER_H

#include <qgsrasterdataprovider.h>
#include <qgsprovidermetadata.h>
#include <QString>

// 本项目输出 GeoTIFF (单波段 CFloat32 SLC/干涉图) 专用 Raster Provider
// URI: insartiff:<percent-encoded 文件路径>
// 复数 → Float32 幅度渲染; 块读取走 GDAL (文件 I/O 层), 统计采样不整图扫描
class InSarTiffRasterProvider : public QgsRasterDataProvider
{
public:
    InSarTiffRasterProvider(const QString& uri,
                            const QgsDataProvider::ProviderOptions& options,
                            Qgis::DataProviderReadFlags flags);
    ~InSarTiffRasterProvider() override;

    QgsRasterDataProvider* clone() const override;
    Qgis::RasterInterfaceCapabilities capabilities() const override;

    QgsRasterBlock* block(int bandNo, const QgsRectangle& extent,
                          int width, int height,
                          QgsRasterBlockFeedback* feedback = nullptr) override;

    int xSize() const override { return mWidth; }
    int ySize() const override { return mHeight; }
    int bandCount() const override { return 1; }
    Qgis::DataType dataType(int bandNo) const override;
    Qgis::DataType sourceDataType(int bandNo) const override;
    QgsRectangle extent() const override;
    QgsCoordinateReferenceSystem crs() const override;

    bool isValid() const override { return mDataset != nullptr; }
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

    // URI 编解码
    static QString buildUri(const QString& path);
    static bool parseUri(const QString& uri, QString* path);

private:
    void* mDataset = nullptr;     // GDALDataset*
    int mWidth = 0;
    int mHeight = 0;
    double mGeoTransform[6] = {};
    bool mHasGeoTransform = false;
    QgsRectangle mExtent;
    QgsCoordinateReferenceSystem mCrs;
    QString mLastError;
};

class InSarTiffProviderMetadata : public QgsProviderMetadata
{
public:
    InSarTiffProviderMetadata();
    QgsDataProvider* createProvider(const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags) override;
};

#endif // INSARTIFFRASTERPROVIDER_H
