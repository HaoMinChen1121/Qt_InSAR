#include "SentinelZipProduct.h"
#include "dataaccess/annotation/SlcAnnotationReader.h"
#include <QFileInfo>
#include <QDebug>
#include <cmath>
#include <algorithm>

namespace insarbg {
QThreadPool* pool()
{
    static QThreadPool* p = []() {
        auto* pool = new QThreadPool;
        pool->setMaxThreadCount(2);
        return pool;
    }();
    return p;
}
} // namespace insarbg

// ── SentinelZipProduct ──

std::shared_ptr<SentinelZipProduct> SentinelZipProduct::open(const QString& zipPath)
{
    auto store = ZipStore::open(zipPath);
    if (!store) return nullptr;

    std::shared_ptr<SentinelZipProduct> p(new SentinelZipProduct);
    p->mZipPath = zipPath;
    p->mStore = store;
    // 缓存预算可调: INSAR_TILE_CACHE_MB (默认 2GB)
    uint64_t budget = 2ull * 1024 * 1024 * 1024;
    if (qEnvironmentVariableIsSet("INSAR_TILE_CACHE_MB")) {
        bool ok = false;
        uint64_t mb = qEnvironmentVariable("INSAR_TILE_CACHE_MB").toULongLong(&ok);
        if (ok && mb > 0) budget = mb * 1024 * 1024;
    }
    p->mTileCache = std::make_shared<TileCache>(budget);
    p->discoverMeasurementEntries();

    // 后台预热: 单个任务串行完整解码各波段 (填充 tile/抽稀/统计),
    // 使用专用线程池, 不与 QGIS 渲染争夺全局线程池
    {
        std::shared_ptr<SentinelZipProduct> self(p);
        insarbg::pool()->start(insarbg::makeRunnable([self]() {
            QStringList entries = self->mMeasurementEntries;
            // VV 优先 (顶层可见波段先获得缓存/统计)
            std::sort(entries.begin(), entries.end(),
                [](const QString& a, const QString& b) {
                    bool av = a.contains(QStringLiteral("-vv"), Qt::CaseInsensitive);
                    bool bv = b.contains(QStringLiteral("-vv"), Qt::CaseInsensitive);
                    if (av != bv) return av;
                    return a < b;
                });
            for (const QString& e : entries) {
                std::shared_ptr<const TiffHeaderInfo> tiff = self->bandTiffInfo(e);
                if (!tiff) continue;
                std::shared_ptr<TiffStreamDecoder> dec = self->bandDecoder(e);
                if (!dec) continue;
                // 分块解码: 每块释放游标, 画布渲染可随时插入
                const int chunk = 2048;
                QVector<float> buf(static_cast<qsizetype>(chunk / 8 + 2)
                                   * ((tiff->width + 7) / 8 + 2));
                for (int y = 0; y < tiff->height; y += chunk) {
                    int hh = std::min(chunk, tiff->height - y);
                    dec->readWindow(0, y, tiff->width, hh,
                                    8, 8, buf.data(), nullptr);
                }
                qDebug() << "[ZipProduct] warm-up done" << e;
            }
        }));
    }
    return p;
}

void SentinelZipProduct::discoverMeasurementEntries()
{
    const QStringList entries = mStore->entryList();
    for (const QString& e : entries) {
        QString l = e.toLower();
        if (l.contains(QStringLiteral("measurement"))
            && (l.endsWith(QStringLiteral(".tiff")) || l.endsWith(QStringLiteral(".tif"))))
            mMeasurementEntries.append(e);
    }
    qDebug() << "[ZipProduct] measurement entries:" << mMeasurementEntries.size()
             << "in" << mZipPath;
}

std::shared_ptr<const TiffHeaderInfo>
SentinelZipProduct::bandTiffInfo(const QString& entry) const
{
    QMutexLocker lock(&mMutex);
    return bandTiffInfoLocked(entry);
}

std::shared_ptr<const TiffHeaderInfo>
SentinelZipProduct::bandTiffInfoLocked(const QString& entry) const
{
    auto it = mTiffInfoCache.find(entry);
    if (it != mTiffInfoCache.end()) return it.value();

    TiffHeaderInfo info;

    // 1) annotation XML (S1 主路径: 尺寸/样本格式/strip起始/地理网格)
    //    注: S1 测量 TIFF 的 IFD 在文件末尾, 头部解析对 S1 无效
    if (!buildFromAnnotation(entry, &info)) {
        // 2) TIFF 头部快速解析 (经典 IFD 布局)
        info = ZipTiffExtractor::extractHeader(mZipPath, entry);
        // 3) 全量解析 (最后手段, 需 inflate 整个 entry)
        if (!info.valid) {
            auto raw = mStore->readEntry(entry);
            if (!raw.empty())
                info = ZipTiffExtractor::parseHeader(raw);
        }
    }

    if (!info.valid) {
        qWarning() << "[ZipProduct] TIFF metadata unavailable:" << entry;
        return nullptr;
    }
    auto shared = std::make_shared<const TiffHeaderInfo>(std::move(info));
    mTiffInfoCache.insert(entry, shared);
    return shared;
}

bool SentinelZipProduct::buildFromAnnotation(const QString& entry,
                                             TiffHeaderInfo* out) const
{
    // 同名 annotation XML: ".../measurement/x.tiff" → ".../annotation/x.xml"
    QString annEntry = entry;
    annEntry.replace(QStringLiteral(".tiff"), QStringLiteral(".xml"),
                     Qt::CaseInsensitive);
    annEntry.replace(QStringLiteral("/measurement/"), QStringLiteral("/annotation/"),
                     Qt::CaseInsensitive);

    const ZipEntryInfo* e = mStore->findEntry(annEntry);
    if (!e) return false;

    std::vector<unsigned char> xml = mStore->readEntry(*e);
    if (xml.empty()) return false;

    SlcAnnotationReader reader;
    if (!reader.openFromData(QByteArray(
            reinterpret_cast<const char*>(xml.data()),
            static_cast<int>(xml.size()))))
        return false;
    SlcAnnotation ann = reader.readAll();
    if (ann.numberOfSamples <= 0 || ann.numberOfLines <= 0) return false;

    TiffHeaderInfo info;
    info.valid = true;
    info.littleEndian = true;
    info.width  = ann.numberOfSamples;
    info.height = ann.numberOfLines;
    info.rowsPerStrip = 1;

    bool complex = ann.pixelValue.contains(QStringLiteral("Complex"),
                                           Qt::CaseInsensitive);
    if (complex) {
        // SLC: SampleFormat=5 (ComplexInt), 每像素 2x int16 = 4 字节
        info.bitsPerSample = 16;
        info.samplesPerPixel = 1;
        info.sampleFormat = 5;
        info.bytesPerRow = static_cast<uint32_t>(info.width) * 4;
    } else {
        // GRD: UInt16, 2 字节/像素
        info.bitsPerSample = 16;
        info.samplesPerPixel = 1;
        info.sampleFormat = 1;
        info.bytesPerRow = static_cast<uint32_t>(info.width) * 2;
    }

    // strip 起始 = 首个 burst 的 byteOffset (TIFF 内字节偏移, 精确);
    // 无 burst (GRD) 时假设 8 (数据紧跟 8 字节头)
    info.firstStripOffset = 8;
    if (!ann.burstList.isEmpty() && ann.burstList[0].byteOffset > 0
        && ann.burstList[0].byteOffset < static_cast<qint64>(1) << 32) {
        info.firstStripOffset = static_cast<uint32_t>(ann.burstList[0].byteOffset);
    }

    // 场景统计预设: annotation imageStatistics 的 I/Q 均值/标准差
    // → 振幅 RMS → Rayleigh 近似 (mean=0.886rms, std=0.463rms)
    if (complex) {
        double sI = ann.stats.outputDataStdDevRe;
        double sQ = ann.stats.outputDataStdDevIm;
        double mI = ann.stats.outputDataMeanRe;
        double mQ = ann.stats.outputDataMeanIm;
        double rms2 = sI * sI + sQ * sQ + mI * mI + mQ * mQ;
        if (rms2 > 0) {
            double rms = std::sqrt(rms2);
            info.presetMean = 0.8862 * rms;
            info.presetStd  = 0.4633 * rms;
            info.presetMax  = std::min(46341.0,
                                       info.presetMean + 3.0 * info.presetStd);
        }
    } else {
        // GRD: 直接使用 DN 统计
        info.presetMean = ann.stats.outputDataMeanRe;
        info.presetStd  = ann.stats.outputDataStdDevRe;
        info.presetMax  = std::min(65535.0,
                                   info.presetMean + 3.0 * info.presetStd);
    }

    // 地理参考: 定位网格 LSQ 拟合 6 参数仿射
    QVector<TiffHeaderInfo::GcpEntry> pts;
    for (const GeolocationPoint& g : ann.geolocationGrid) {
        TiffHeaderInfo::GcpEntry p;
        p.pixel = g.pixel; p.line = g.line;
        p.lon = g.longitude; p.lat = g.latitude; p.height = g.height;
        pts.append(p);
    }
    if (pts.size() >= 4
        && ZipTiffExtractor::fitGeoTransform(pts, info.geoTransform)) {
        info.hasGeoTransform = true;
        info.gcps = pts;
    }
    info.projectionWkt = ZipTiffExtractor::wgs84Wkt();

    *out = std::move(info);
    qDebug() << "[ZipProduct] TiffInfo from annotation" << entry
             << out->width << "x" << out->height
             << "stripStart=" << out->firstStripOffset
             << "bytesPerRow=" << out->bytesPerRow;
    return true;
}

std::shared_ptr<TiffStreamDecoder>
SentinelZipProduct::bandDecoder(const QString& entry) const
{
    QMutexLocker lock(&mMutex);
    auto it = mDecoderCache.find(entry);
    if (it != mDecoderCache.end()) return it.value();

    std::shared_ptr<const TiffHeaderInfo> tiff = bandTiffInfoLocked(entry);
    if (!tiff) return nullptr;

    auto decoder = std::make_shared<TiffStreamDecoder>(
        mStore, entry, tiff, mTileCache);
    mDecoderCache.insert(entry, decoder);
    return decoder;
}

// ── SentinelProductManager ──

SentinelProductManager& SentinelProductManager::instance()
{
    static SentinelProductManager mgr;
    return mgr;
}

std::shared_ptr<SentinelZipProduct> SentinelProductManager::acquire(const QString& zipPath)
{
    QString key = QFileInfo(zipPath).absoluteFilePath().toLower();
    QMutexLocker lock(&mMutex);

    auto it = mProducts.find(key);
    if (it == mProducts.end()) {
        auto p = SentinelZipProduct::open(zipPath);
        if (!p) return nullptr;
        it = mProducts.insert(key, p);
        mRefCounts.insert(key, 0);
    }
    mRefCounts[key]++;
    return it.value();
}

void SentinelProductManager::release(const QString& zipPath)
{
    QString key = QFileInfo(zipPath).absoluteFilePath().toLower();
    QMutexLocker lock(&mMutex);
    auto it = mRefCounts.find(key);
    if (it == mRefCounts.end()) return;
    if (--it.value() <= 0) {
        mRefCounts.erase(it);
        mProducts.remove(key);
        qDebug() << "[ProductManager] released" << zipPath;
    }
}

void SentinelProductManager::clear()
{
    QMutexLocker lock(&mMutex);
    mProducts.clear();
    mRefCounts.clear();
}
