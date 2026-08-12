#include "Sentinel1RasterProvider.h"

#include "dataaccess/impl/SentinelZipProduct.h"
#include "dataaccess/impl/ZipTiffExtractor.h"
#include "dataaccess/impl/TiffStreamDecoder.h"

#include <qgsrasterblock.h>
#include <qgsrasterbandstats.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsrectangle.h>
#include <qgsapplication.h>
#include <QUrl>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>

// ── URI 编解码 ──
QString Sentinel1RasterProvider::buildUri(const QString& zipPath,
                                          const QString& entry,
                                          const QString& mode)
{
    QString payload = zipPath + QStringLiteral("|") + entry
                    + QStringLiteral("|") + mode;
    return QStringLiteral("sentinel1zip:")
         + QString::fromLatin1(QUrl::toPercentEncoding(payload));
}

bool Sentinel1RasterProvider::parseUri(const QString& uri,
                                       QString* zipPath, QString* entry,
                                       QString* mode)
{
    const QString prefix = QStringLiteral("sentinel1zip:");
    if (!uri.startsWith(prefix)) return false;
    QString payload = QString::fromUtf8(
        QByteArray::fromPercentEncoding(uri.mid(prefix.size()).toUtf8()));
    QStringList parts = payload.split('|');
    if (parts.size() < 2) return false;
    *zipPath = parts[0];
    *entry  = parts[1];
    *mode   = parts.size() >= 3 ? parts[2] : QStringLiteral("amplitude");
    return !zipPath->isEmpty() && !entry->isEmpty();
}

// ── Provider ──
Sentinel1RasterProvider::Sentinel1RasterProvider(
        const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags)
    : QgsRasterDataProvider(uri, options, flags)
{
    if (!parseUri(uri, &mZipPath, &mEntry, &mMode)) {
        mLastError = QStringLiteral("无效的 sentinel1zip URI: %1").arg(uri);
        qWarning() << "[S1Provider]" << mLastError;
        return;
    }

    mProduct = SentinelProductManager::instance().acquire(mZipPath);
    if (!mProduct) {
        mLastError = QStringLiteral("无法打开 ZIP: %1").arg(mZipPath);
        qWarning() << "[S1Provider]" << mLastError;
        return;
    }

    mTiff = mProduct->bandTiffInfo(mEntry);
    if (!mTiff) {
        mLastError = QStringLiteral("TIFF 头解析失败: %1").arg(mEntry);
        qWarning() << "[S1Provider]" << mLastError;
        SentinelProductManager::instance().release(mZipPath);
        mProduct.reset();
        return;
    }

    mDecoder = mProduct->bandDecoder(mEntry);

    // 地理参考 (完整仿射, 支持旋转项)
    double gt[6];
    if (mTiff->getGeoTransform(gt)) {
        auto cornerX = [&](double pxx, double pyy) { return gt[0] + gt[1] * pxx + gt[2] * pyy; };
        auto cornerY = [&](double pxx, double pyy) { return gt[3] + gt[4] * pxx + gt[5] * pyy; };
        const double w = mTiff->width, h = mTiff->height;
        double xs[4] = { cornerX(0, 0), cornerX(w, 0), cornerX(0, h), cornerX(w, h) };
        double ys[4] = { cornerY(0, 0), cornerY(w, 0), cornerY(0, h), cornerY(w, h) };
        mExtent = QgsRectangle(
            std::min({xs[0], xs[1], xs[2], xs[3]}),
            std::min({ys[0], ys[1], ys[2], ys[3]}),
            std::max({xs[0], xs[1], xs[2], xs[3]}),
            std::max({ys[0], ys[1], ys[2], ys[3]}));
    }
    if (!mTiff->projectionWkt.isEmpty())
        mCrs = QgsCoordinateReferenceSystem::fromWkt(mTiff->projectionWkt);
    if (!mCrs.isValid())
        mCrs = QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326"));

    // 默认 NODATA = 0 (SLC/GRD 边缘)
    setNoDataValue(1, 0.0);

    qDebug() << "[S1Provider] opened" << mEntry << mTiff->width << "x"
             << mTiff->height << "strips=" << mTiff->stripOffsets.size()
             << "mode=" << mMode;
}

Sentinel1RasterProvider::~Sentinel1RasterProvider()
{
    if (mProduct)
        SentinelProductManager::instance().release(mZipPath);
}

std::shared_ptr<TiffStreamDecoder> Sentinel1RasterProvider::bandDecoder() const
{
    return mDecoder;
}

QgsRasterDataProvider* Sentinel1RasterProvider::clone() const
{
    return new Sentinel1RasterProvider(dataSourceUri(),
        QgsDataProvider::ProviderOptions(), Qgis::DataProviderReadFlags());
}

Qgis::RasterInterfaceCapabilities Sentinel1RasterProvider::capabilities() const
{
    return Qgis::RasterInterfaceCapability::IdentifyValue;
}

QString Sentinel1RasterProvider::name() const
{
    return QStringLiteral("sentinel1zip");
}

QString Sentinel1RasterProvider::description() const
{
    return dataSourceUri();
}

Qgis::DataType Sentinel1RasterProvider::dataType(int bandNo) const
{
    Q_UNUSED(bandNo)
    return Qgis::DataType::Float32;
}

Qgis::DataType Sentinel1RasterProvider::sourceDataType(int bandNo) const
{
    Q_UNUSED(bandNo)
    if (!mTiff) return Qgis::DataType::Float32;
    if (mTiff->sampleFormat == 3) return Qgis::DataType::CFloat32;
    return mTiff->isComplex() ? Qgis::DataType::CInt16 : Qgis::DataType::UInt16;
}

QgsRectangle Sentinel1RasterProvider::extent() const
{
    return mExtent;
}

QgsCoordinateReferenceSystem Sentinel1RasterProvider::crs() const
{
    return mCrs;
}

bool Sentinel1RasterProvider::setNoDataValue(int bandNo, double noDataValue)
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
QgsRasterBlock* Sentinel1RasterProvider::block(int bandNo,
                                               const QgsRectangle& extent,
                                               int width, int height,
                                               QgsRasterBlockFeedback* feedback)
{
    if (bandNo != 1 || !mDecoder || width <= 0 || height <= 0 || extent.isEmpty()) {
        QgsRasterBlock* b = new QgsRasterBlock(Qgis::DataType::Float32,
                                               std::max(1, width),
                                               std::max(1, height));
        b->setValid(true);
        return b;
    }

    double gt[6];
    if (!mTiff->getGeoTransform(gt)) return nullptr;

    // extent → 源像素窗口 (完整仿射, 支持旋转项)
    // lon = gt0 + gt1*px + gt2*py; lat = gt3 + gt4*px + gt5*py
    double det = gt[1] * gt[5] - gt[2] * gt[4];
    if (det == 0.0) return nullptr;
    auto px = [&](double x, double y) {
        return ((x - gt[0]) * gt[5] - gt[2] * (y - gt[3])) / det;
    };
    auto py = [&](double x, double y) {
        return (gt[1] * (y - gt[3]) - (x - gt[0]) * gt[4]) / det;
    };
    const double X0 = extent.xMinimum(), X1 = extent.xMaximum();
    const double Y0 = extent.yMinimum(), Y1 = extent.yMaximum();
    double sx0 = std::min({px(X0, Y0), px(X1, Y0), px(X0, Y1), px(X1, Y1)});
    double sx1 = std::max({px(X0, Y0), px(X1, Y0), px(X0, Y1), px(X1, Y1)});
    double sy0 = std::min({py(X0, Y0), py(X1, Y0), py(X0, Y1), py(X1, Y1)});
    double sy1 = std::max({py(X0, Y0), py(X1, Y0), py(X0, Y1), py(X1, Y1)});

    int x0 = static_cast<int>(std::floor(sx0));
    int x1 = static_cast<int>(std::ceil(sx1));
    int y0 = static_cast<int>(std::floor(sy0));
    int y1 = static_cast<int>(std::ceil(sy1));

    // 裁剪
    x0 = std::max(0, std::min(x0, xSize()));
    x1 = std::max(0, std::min(x1, xSize()));
    y0 = std::max(0, std::min(y0, ySize()));
    y1 = std::max(0, std::min(y1, ySize()));

    QgsRasterBlock* blk = new QgsRasterBlock(Qgis::DataType::Float32, width, height);
    float* out = reinterpret_cast<float*>(blk->bits());

    if (x1 <= x0 || y1 <= y0) {
        std::fill(out, out + static_cast<qsizetype>(width) * height, 0.0f);
        blk->setValid(true);
        return blk;
    }

    int srcW = x1 - x0;
    int srcH = y1 - y0;
    int xStride = std::max(1, (srcW + width - 1) / width);
    int yStride = std::max(1, (srcH + height - 1) / height);
    int nCols = std::min(width, (srcW + xStride - 1) / xStride);
    int nRows = std::min(height, (srcH + yStride - 1) / yStride);

    QVector<float> tmp(static_cast<qsizetype>(nRows) * nCols);
    // 分块请求: 每块最多 256 输出行, 单次游标持有时间受限,
    // 被取消的请求/其他渲染可及时插入 (避免 canvas 交互卡顿)
    int got = 0;
    const int chunkRows = 256;
    for (int done = 0; done < nRows; ) {
        int cnt = std::min(chunkRows, nRows - done);
        int r0 = y0 + done * yStride;
        int rh = cnt * yStride;
        int g = mDecoder->readWindow(x0, r0, srcW, rh, yStride, xStride,
                                     tmp.data() + static_cast<qsizetype>(done) * nCols,
                                     feedback);
        if (g < 0) {
            delete blk;
            return nullptr;
        }
        got += g;
        done += g;
        if (g < cnt) break;   // 取消或数据截断
    }
    if (got == 0) {
        // 渲染被取消 → 返回空块, QGIS 保留上一帧而不是画成全透明
        if (feedback && feedback->isCanceled()) {
            delete blk;
            return nullptr;
        }
        std::fill(out, out + static_cast<qsizetype>(width) * height, 0.0f);
        blk->setValid(true);
        return blk;
    }

    // 逐像素逆仿射采样: 输出像素 (j,i) → CRS 坐标 → 源像素 → 最近邻
    // (保留旋转项, 使画布中影像保持倾斜的雷达坐标系)
    const double cellX = extent.width() / width;
    const double cellY = extent.height() / height;
    for (int i = 0; i < height; ++i) {
        double crsY = extent.yMaximum() - (i + 0.5) * cellY;   // row 0 = 北
        float* outRow = out + static_cast<qsizetype>(i) * width;
        for (int j = 0; j < width; ++j) {
            double crsX = extent.xMinimum() + (j + 0.5) * cellX;
            double spx = px(crsX, crsY);
            double spy = py(crsX, crsY);
            int r = static_cast<int>(std::lround((spy - y0) / yStride));
            int c = static_cast<int>(std::lround((spx - x0) / xStride));
            if (r < 0 || r >= got || c < 0 || c >= nCols) {
                outRow[j] = 0.0f;
            } else {
                outRow[j] = tmp[static_cast<qsizetype>(r) * nCols + c];
            }
        }
    }

    blk->setNoDataValue(0.0);
    blk->setValid(true);
    return blk;
}

// ── 统计: 采样, 避免全图扫描 ──
QgsRasterBandStats Sentinel1RasterProvider::bandStatistics(
        int bandNo, Qgis::RasterBandStatistics stats,
        const QgsRectangle& extent, int sampleSize,
        QgsRasterBlockFeedback* feedback)
{
    Q_UNUSED(stats)
    QgsRasterBandStats s;
    if (bandNo != 1 || !mDecoder) return s;

    bool whole = extent.isEmpty() || extent == mExtent;
    double mn = 0, mx = 0, mean = 0, std = 0;
    uint64_t cnt = 0;

    if (whole) {
        int maxRows = sampleSize > 0
            ? std::max(64, std::min(512, static_cast<int>(std::sqrt(sampleSize))))
            : 256;
        if (!mDecoder->sampledStats(mn, mx, mean, std, maxRows, feedback))
            return s;
        s.elementCount = static_cast<qgssize>(xSize()) * ySize();
        s.minimumValue = mn;
        s.maximumValue = mx;
        s.range = mx - mn;
        s.mean = mean;
        s.stdDev = std;
        s.statsGathered = Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max
                        | Qgis::RasterBandStatistic::Range | Qgis::RasterBandStatistic::Mean
                        | Qgis::RasterBandStatistic::StdDev;
        return s;
    } else {
        // 局部统计: 采样一个小块
        QgsRasterBlock* blk = block(1, extent, 256, 256, feedback);
        if (!blk) return s;
        const float* p = reinterpret_cast<const float*>(blk->bits());
        qsizetype n = static_cast<qsizetype>(blk->width()) * blk->height();
        double sum = 0, sumsq = 0;
        for (qsizetype i = 0; i < n; ++i) {
            float v = p[i];
            if (v <= 0.0f) continue;
            if (cnt == 0) { mn = mx = v; }
            else { mn = std::min(mn, static_cast<double>(v)); mx = std::max(mx, static_cast<double>(v)); }
            sum += v; sumsq += static_cast<double>(v) * v;
            ++cnt;
        }
        delete blk;
        if (cnt == 0) return s;
        mean = sum / cnt;
        double var = sumsq / cnt - mean * mean;
        std = var > 0 ? std::sqrt(var) : 0.0;
    }

    if (cnt == 0) return s;
    s.elementCount = static_cast<qgssize>(std::max<uint64_t>(cnt, 1));
    s.minimumValue = mn;
    s.maximumValue = mx;
    s.range = mx - mn;
    s.mean = mean;
    s.stdDev = std;
    s.statsGathered = Qgis::RasterBandStatistic::Min | Qgis::RasterBandStatistic::Max
                    | Qgis::RasterBandStatistic::Range | Qgis::RasterBandStatistic::Mean
                    | Qgis::RasterBandStatistic::StdDev;
    return s;
}

// ── ProviderMetadata ──
Sentinel1ProviderMetadata::Sentinel1ProviderMetadata()
    : QgsProviderMetadata(QStringLiteral("sentinel1zip"),
          QStringLiteral("Sentinel-1 ZIP 产品 (SNAP 式直接读取)"))
{
}

QgsDataProvider* Sentinel1ProviderMetadata::createProvider(
        const QString& uri,
        const QgsDataProvider::ProviderOptions& options,
        Qgis::DataProviderReadFlags flags)
{
    return new Sentinel1RasterProvider(uri, options, flags);
}
