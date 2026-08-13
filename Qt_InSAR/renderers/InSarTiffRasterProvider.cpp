#include "InSarTiffRasterProvider.h"

#include <qgsrasterblock.h>
#include <qgsrasterbandstats.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsrectangle.h>

#include <gdal_priv.h>

#include <QUrl>
#include <QDebug>
#include <QVector>
#include <cmath>
#include <algorithm>
#include <complex>
#include <limits>

// ── URI 编解码 ──
QString InSarTiffRasterProvider::buildUri(const QString& path)
{
    return QStringLiteral("insartiff:")
         + QString::fromLatin1(QUrl::toPercentEncoding(path));
}

bool InSarTiffRasterProvider::parseUri(const QString& uri, QString* path)
{
    const QString prefix = QStringLiteral("insartiff:");
    if (!uri.startsWith(prefix)) return false;
    *path = QString::fromUtf8(
        QByteArray::fromPercentEncoding(uri.mid(prefix.size()).toUtf8()));
    return !path->isEmpty();
}

// ── Provider ──
InSarTiffRasterProvider::InSarTiffRasterProvider(
        const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags)
    : QgsRasterDataProvider(uri, options, flags)
{
    QString path;
    if (!parseUri(uri, &path)) {
        mLastError = QStringLiteral("无效的 insartiff URI: %1").arg(uri);
        qWarning() << "[InSarTiff]" << mLastError;
        return;
    }

    GDALDatasetH hDS = GDALOpenEx(path.toUtf8().constData(),
        GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!hDS) {
        mLastError = QStringLiteral("无法打开: %1").arg(path);
        qWarning() << "[InSarTiff]" << mLastError;
        return;
    }
    mDataset = hDS;

    mWidth  = GDALGetRasterXSize(hDS);
    mHeight = GDALGetRasterYSize(hDS);

    // 地理变换: 优先 GeoTransform; GCP 数据 (SLC tiepoints) 回退仿射近似
    if (GDALGetGeoTransform(hDS, mGeoTransform) == CE_None) {
        mHasGeoTransform = true;
    } else if (GDALGetGCPCount(hDS) > 0) {
        mHasGeoTransform = (GDALGCPsToGeoTransform(GDALGetGCPCount(hDS),
            GDALGetGCPs(hDS), mGeoTransform, TRUE) == TRUE);
    }

    if (mHasGeoTransform) {
        auto cornerX = [&](double px, double py) {
            return mGeoTransform[0] + mGeoTransform[1] * px + mGeoTransform[2] * py;
        };
        auto cornerY = [&](double px, double py) {
            return mGeoTransform[3] + mGeoTransform[4] * px + mGeoTransform[5] * py;
        };
        const double w = mWidth, h = mHeight;
        double xs[4] = { cornerX(0, 0), cornerX(w, 0), cornerX(0, h), cornerX(w, h) };
        double ys[4] = { cornerY(0, 0), cornerY(w, 0), cornerY(0, h), cornerY(w, h) };
        mExtent = QgsRectangle(
            std::min({xs[0], xs[1], xs[2], xs[3]}),
            std::min({ys[0], ys[1], ys[2], ys[3]}),
            std::max({xs[0], xs[1], xs[2], xs[3]}),
            std::max({ys[0], ys[1], ys[2], ys[3]}));
    } else {
        mExtent = QgsRectangle(0, 0, mWidth, mHeight);
    }

    const char* gcpWkt = GDALGetGCPProjection(hDS);
    const char* wkt = GDALGetProjectionRef(hDS);
    if (gcpWkt && gcpWkt[0])
        mCrs = QgsCoordinateReferenceSystem::fromWkt(QString::fromUtf8(gcpWkt));
    else if (wkt && wkt[0])
        mCrs = QgsCoordinateReferenceSystem::fromWkt(QString::fromUtf8(wkt));
    if (!mCrs.isValid())
        mCrs = QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326"));

    // 默认 NODATA = 0 (SLC 边缘)
    setNoDataValue(1, 0.0);

    qDebug() << "[InSarTiff] opened" << path << mWidth << "x" << mHeight
             << "hasGeoTransform=" << mHasGeoTransform;
}

InSarTiffRasterProvider::~InSarTiffRasterProvider()
{
    if (mDataset) {
        GDALClose(static_cast<GDALDatasetH>(mDataset));
        mDataset = nullptr;
    }
}

QgsRasterDataProvider* InSarTiffRasterProvider::clone() const
{
    return new InSarTiffRasterProvider(dataSourceUri(),
        QgsDataProvider::ProviderOptions(), Qgis::DataProviderReadFlags());
}

Qgis::RasterInterfaceCapabilities InSarTiffRasterProvider::capabilities() const
{
    return Qgis::RasterInterfaceCapability::IdentifyValue;
}

QString InSarTiffRasterProvider::name() const
{
    return QStringLiteral("insartiff");
}

QString InSarTiffRasterProvider::description() const
{
    return dataSourceUri();
}

Qgis::DataType InSarTiffRasterProvider::dataType(int bandNo) const
{
    Q_UNUSED(bandNo)
    return Qgis::DataType::Float32;
}

Qgis::DataType InSarTiffRasterProvider::sourceDataType(int bandNo) const
{
    Q_UNUSED(bandNo)
    return Qgis::DataType::CFloat32;
}

QgsRectangle InSarTiffRasterProvider::extent() const
{
    return mExtent;
}

QgsCoordinateReferenceSystem InSarTiffRasterProvider::crs() const
{
    return mCrs;
}

bool InSarTiffRasterProvider::setNoDataValue(int bandNo, double noDataValue)
{
    if (bandNo != 1) return false;
    while (mSrcNoDataValue.size() < 1) {
        mSrcNoDataValue.append(std::numeric_limits<double>::quiet_NaN());
        mSrcHasNoDataValue.append(false);
        mUseSrcNoDataValue.append(false);
    }
    mSrcNoDataValue[0] = noDataValue;
    mSrcHasNoDataValue[0] = true;
    mUseSrcNoDataValue[0] = true;
    return true;
}

// ── block: QGIS 渲染核心 ──
QgsRasterBlock* InSarTiffRasterProvider::block(int bandNo,
                                               const QgsRectangle& extent,
                                               int width, int height,
                                               QgsRasterBlockFeedback* feedback)
{
    if (bandNo != 1 || !mDataset || width <= 0 || height <= 0 || extent.isEmpty()) {
        QgsRasterBlock* b = new QgsRasterBlock(Qgis::DataType::Float32,
                                               std::max(1, width),
                                               std::max(1, height));
        b->setValid(true);
        return b;
    }

    QgsRasterBlock* blk = new QgsRasterBlock(Qgis::DataType::Float32, width, height);
    float* out = reinterpret_cast<float*>(blk->bits());
    std::fill(out, out + static_cast<qsizetype>(width) * height, 0.0f);

    // extent → 源像素窗口
    double sx0, sx1, sy0, sy1;
    if (mHasGeoTransform) {
        double gt[6];
        std::copy(mGeoTransform, mGeoTransform + 6, gt);
        double det = gt[1] * gt[5] - gt[2] * gt[4];
        if (det == 0.0) { blk->setValid(true); return blk; }
        auto px = [&](double x, double y) {
            return ((x - gt[0]) * gt[5] - gt[2] * (y - gt[3])) / det;
        };
        auto py = [&](double x, double y) {
            return (gt[1] * (y - gt[3]) - (x - gt[0]) * gt[4]) / det;
        };
        const double X0 = extent.xMinimum(), X1 = extent.xMaximum();
        const double Y0 = extent.yMinimum(), Y1 = extent.yMaximum();
        sx0 = std::min({px(X0, Y0), px(X1, Y0), px(X0, Y1), px(X1, Y1)});
        sx1 = std::max({px(X0, Y0), px(X1, Y0), px(X0, Y1), px(X1, Y1)});
        sy0 = std::min({py(X0, Y0), py(X1, Y0), py(X0, Y1), py(X1, Y1)});
        sy1 = std::max({py(X0, Y0), py(X1, Y0), py(X0, Y1), py(X1, Y1)});
    } else {
        // 无地理参考: extent 即像素空间 (北向上翻转)
        sx0 = extent.xMinimum();
        sx1 = extent.xMaximum();
        sy0 = mHeight - extent.yMaximum();
        sy1 = mHeight - extent.yMinimum();
    }

    int x0 = std::max(0, std::min(static_cast<int>(std::floor(sx0)), mWidth));
    int x1 = std::max(0, std::min(static_cast<int>(std::ceil(sx1)), mWidth));
    int y0 = std::max(0, std::min(static_cast<int>(std::floor(sy0)), mHeight));
    int y1 = std::max(0, std::min(static_cast<int>(std::ceil(sy1)), mHeight));

    if (x1 <= x0 || y1 <= y0) { blk->setValid(true); return blk; }

    const int srcW = x1 - x0;
    const int srcH = y1 - y0;
    const int xStride = std::max(1, (srcW + width - 1) / width);
    const int yStride = std::max(1, (srcH + height - 1) / height);
    const int nCols = std::min(width, (srcW + xStride - 1) / xStride);
    const int nRows = std::min(height, (srcH + yStride - 1) / yStride);

    // 一次 GDAL RasterIO: 源窗口 → 降采样网格 (GDAL 最近邻)
    QVector<std::complex<float>> tmp(static_cast<qsizetype>(nRows) * nCols);
    CPLErr err = GDALRasterIO(GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 1),
        GF_Read, x0, y0, srcW, srcH, tmp.data(), nCols, nRows, GDT_CFloat32, 0, 0);
    if (err != CE_None) {
        blk->setValid(true);
        return blk;
    }

    // 幅度
    QVector<float> mag(static_cast<qsizetype>(nRows) * nCols);
    for (qsizetype i = 0; i < tmp.size(); ++i)
        mag[i] = std::abs(tmp[i]);

    // 逐像素最近邻映射 (保留旋转项, 与 sentinel1zip 行为一致)
    const double cellX = extent.width() / width;
    const double cellY = extent.height() / height;
    double gt[6] = {};
    double det = 0.0;
    if (mHasGeoTransform) {
        std::copy(mGeoTransform, mGeoTransform + 6, gt);
        det = gt[1] * gt[5] - gt[2] * gt[4];
        if (det == 0.0) { blk->setValid(true); return blk; }
    }
    auto toSrcX = [&](double x, double y) {
        return mHasGeoTransform
            ? ((x - gt[0]) * gt[5] - gt[2] * (y - gt[3])) / det : x;
    };
    auto toSrcY = [&](double x, double y) {
        return mHasGeoTransform
            ? (gt[1] * (y - gt[3]) - (x - gt[0]) * gt[4]) / det : mHeight - y;
    };
    for (int i = 0; i < height; ++i) {
        double crsY = extent.yMaximum() - (i + 0.5) * cellY;
        float* outRow = out + static_cast<qsizetype>(i) * width;
        for (int j = 0; j < width; ++j) {
            double crsX = extent.xMinimum() + (j + 0.5) * cellX;
            double spx = toSrcX(crsX, crsY);
            double spy = toSrcY(crsX, crsY);
            int r = static_cast<int>(std::lround((spy - y0) / yStride));
            int c = static_cast<int>(std::lround((spx - x0) / xStride));
            if (r < 0 || r >= nRows || c < 0 || c >= nCols)
                outRow[j] = 0.0f;
            else
                outRow[j] = mag[static_cast<qsizetype>(r) * nCols + c];
        }
    }

    blk->setNoDataValue(0.0);
    blk->setValid(true);
    return blk;
}

// ── 统计: 分散采样窗口, 避免整图扫描 (大文件加载不可阻塞) ──
QgsRasterBandStats InSarTiffRasterProvider::bandStatistics(
        int bandNo, Qgis::RasterBandStatistics stats,
        const QgsRectangle& extent, int sampleSize,
        QgsRasterBlockFeedback* feedback)
{
    Q_UNUSED(stats)
    Q_UNUSED(sampleSize)
    QgsRasterBandStats s;
    if (bandNo != 1 || !mDataset) return s;

    const int win = std::min(256, std::min(mWidth, mHeight));
    const int nWin = 16;

    double mn = 1e300, mx = -1e300, sum = 0, sumsq = 0;
    uint64_t cnt = 0;
    QVector<std::complex<float>> buf(static_cast<qsizetype>(win) * win);
    for (int i = 0; i < nWin; ++i) {
        int wx = static_cast<int>((mWidth - win) * ((i * 37 + 11) % 97) / 97.0);
        int wy = static_cast<int>((mHeight - win) * ((i * 53 + 7) % 89) / 89.0);
        if (GDALRasterIO(GDALGetRasterBand(static_cast<GDALDatasetH>(mDataset), 1),
                GF_Read, wx, wy, win, win, buf.data(), win, win,
                GDT_CFloat32, 0, 0) != CE_None)
            continue;
        for (const auto& v : buf) {
            double m = std::abs(v);
            if (m <= 0.0) continue;
            if (m < mn) mn = m;
            if (m > mx) mx = m;
            sum += m;
            sumsq += m * m;
            ++cnt;
        }
    }

    if (cnt == 0) return s;
    double mean = sum / cnt;
    double var = sumsq / cnt - mean * mean;
    s.elementCount = static_cast<qgssize>(xSize()) * ySize();
    s.minimumValue = mn;
    s.maximumValue = mx;
    s.range = mx - mn;
    s.mean = mean;
    s.stdDev = var > 0 ? std::sqrt(var) : 0.0;
    s.statsGathered = Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max
                    | Qgis::RasterBandStatistic::Range | Qgis::RasterBandStatistic::Mean
                    | Qgis::RasterBandStatistic::StdDev;
    return s;
}

// ── ProviderMetadata ──
InSarTiffProviderMetadata::InSarTiffProviderMetadata()
    : QgsProviderMetadata(QStringLiteral("insartiff"),
          QStringLiteral("InSAR 输出 GeoTIFF (复数→幅度渲染)"))
{
}

QgsDataProvider* InSarTiffProviderMetadata::createProvider(
        const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags)
{
    return new InSarTiffRasterProvider(uri, options, flags);
}
