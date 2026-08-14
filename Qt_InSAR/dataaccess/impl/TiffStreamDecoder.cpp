#include "TiffStreamDecoder.h"
#include <qgsrasterinterface.h>   // QgsRasterBlockFeedback
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QMutexLocker>
#include <cmath>
#include <cstring>
#include <algorithm>

TiffStreamDecoder::TiffStreamDecoder(std::shared_ptr<ZipStore> store,
                                     const QString& entryName,
                                     std::shared_ptr<const TiffHeaderInfo> tiff,
                                     std::shared_ptr<TileCache> cache)
    : mStore(std::move(store))
    , mEntryName(entryName)
    , mTiff(std::move(tiff))
    , mCache(std::move(cache))
{
    mCacheKey = mStore->zipPath() + QStringLiteral("|") + mEntryName;
    mRawRow.resize(mTiff->bytesPerRow > 0 ? mTiff->bytesPerRow : 1);
    mSkipBuf.resize(256 * 1024);
    mRowF.resize(mTiff->width > 0 ? mTiff->width : 1);
}

// ── 单行原始字节 → 全宽 Float32 ──
void TiffStreamDecoder::convertRow(const uint8_t* raw, float* rowF)
{
    const int w = mTiff->width;
    const bool le = mTiff->littleEndian;
    const int spp = mTiff->samplesPerPixel;
    const int fmt = mTiff->sampleFormat;
    const int bps = mTiff->bitsPerSample;

    auto r16 = [le](const uint8_t* p) -> int16_t {
        return le ? static_cast<int16_t>(p[0] | (p[1] << 8))
                  : static_cast<int16_t>((p[0] << 8) | p[1]);
    };
    auto ru16 = [le](const uint8_t* p) -> uint16_t {
        return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                  : static_cast<uint16_t>((p[0] << 8) | p[1]);
    };
    auto rf32 = [le](const uint8_t* p) -> float {
        uint32_t v = le ? (p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24))
                        : ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
        float f; std::memcpy(&f, &v, 4); return f;
    };

    // 复数判定: spp>=2, 或 SampleFormat 5(ComplexInt)/6(ComplexFloat)/3(float, spp>=2)
    if (spp >= 2 || fmt == 5 || fmt == 6) {
        if (fmt == 3 || fmt == 6) {
            // CFloat32 / ComplexFloat: 2x float32 = 8 字节/像素
            for (int c = 0; c < w; ++c) {
                float I = rf32(raw + c * 8);
                float Q = rf32(raw + c * 8 + 4);
                rowF[c] = std::hypot(I, Q);
            }
        } else {
            // CInt16 / ComplexInt: 2x int16 = 4 字节/像素
            for (int c = 0; c < w; ++c) {
                float I = static_cast<float>(r16(raw + c * 4));
                float Q = static_cast<float>(r16(raw + c * 4 + 2));
                rowF[c] = std::hypot(I, Q);
            }
        }
        return;
    }

    // 实数 → 直通
    if (fmt == 3) {
        for (int c = 0; c < w; ++c)
            rowF[c] = rf32(raw + c * 4);
    } else if (bps == 16 && fmt <= 1) {
        for (int c = 0; c < w; ++c)
            rowF[c] = static_cast<float>(ru16(raw + c * 2));
    } else if (bps == 16) {
        for (int c = 0; c < w; ++c)
            rowF[c] = static_cast<float>(r16(raw + c * 2));
    } else {
        for (int c = 0; c < w; ++c)
            rowF[c] = static_cast<float>(raw[c]);
    }
}

// ── tile 暂存带 ──
void TiffStreamDecoder::flushStaging()
{
    if (mStagingRows <= 0 || mStagingTy < 0) return;
    const int w = mTiff->width;
    const int nXTiles = (w + kTileSize - 1) / kTileSize;

    for (int tx = 0; tx < nXTiles; ++tx) {
        int tileW = std::min(kTileSize, w - tx * kTileSize);
        auto tile = std::make_shared<TileCache::Tile>();
        tile->width = tileW;
        tile->height = mStagingRows;
        tile->data.resize(static_cast<qsizetype>(tileW) * mStagingRows);
        for (int r = 0; r < mStagingRows; ++r) {
            std::memcpy(tile->data.data() + static_cast<qsizetype>(r) * tileW,
                        mStaging.constData() + static_cast<qsizetype>(r) * w + tx * kTileSize,
                        static_cast<size_t>(tileW) * sizeof(float));
        }
        mCache->put(mCacheKey, tx, mStagingTy, tile);
    }
    mStagingRows = 0;
}

void TiffStreamDecoder::stageRow(int row, const float* rowF)
{
    const int w = mTiff->width;
    if (mStaging.isEmpty()) {
        mStaging.resize(static_cast<qsizetype>(kTileSize) * w);
        mStagingTy = -1;
    }
    int ty = row / kTileSize;
    if (ty != mStagingTy) {
        flushStaging();
        mStagingTy = ty;
    }
    int ry = row % kTileSize;
    std::memcpy(mStaging.data() + static_cast<qsizetype>(ry) * w,
                rowF, static_cast<size_t>(w) * sizeof(float));
    ++mStagingRows;
    if (mStagingRows == kTileSize)
        flushStaging();
}

// ── 顺序解码 ──
int TiffStreamDecoder::decodeRows(int maxRow,
                                  const QVector<int>& wantedRows,
                                  int x0, int w, int xStride,
                                  float* dst, QgsRasterBlockFeedback* feedback)
{
    if (!mStream) {
        mStream.reset(mStore->openInflateStream(mEntryName));
        if (!mStream) return -1;
        mInflatedRows = 0;
        mUoff = 0;
    }

    const int width = mTiff->width;
    const int rowsPerStrip = std::max(1, mTiff->rowsPerStrip);
    const uint64_t bytesPerRow = std::max<uint64_t>(1, mTiff->bytesPerRow);
    const int nOutCols = (w + xStride - 1) / xStride;

    int wantedIdx = 0;
    int outIdx = 0;
    int r = mInflatedRows;

    for (; r <= maxRow; ++r) {
        if (feedback && feedback->isCanceled())
            break;

        // 定位行数据
        int stripIdx = r / rowsPerStrip;
        uint64_t rowOff = 0;
        bool rowValid = true;
        if (stripIdx < mTiff->stripOffsets.size()) {
            rowOff = mTiff->stripOffsets[stripIdx]
                   + static_cast<uint64_t>(r % rowsPerStrip) * bytesPerRow;
            if (stripIdx < mTiff->stripByteCounts.size()) {
                uint64_t inStrip = static_cast<uint64_t>(r % rowsPerStrip) * bytesPerRow;
                if (inStrip + bytesPerRow > mTiff->stripByteCounts[stripIdx])
                    rowValid = false;
            }
        } else {
            rowOff = mTiff->firstStripOffset + static_cast<uint64_t>(r) * bytesPerRow;
        }

        if (rowOff < mUoff) {
            // strip 偏移非单调 (不应出现在 S1 产品), 保持前进
            qWarning() << "[Decoder] non-monotonic strip offset row" << r;
            rowOff = mUoff;
        }

        // 跳过 strip 间隙
        uint64_t skip = rowOff - mUoff;
        while (skip > 0) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(skip, mSkipBuf.size()));
            int got = mStream->produce(mSkipBuf.data(), n);
            if (got <= 0) { skip = 0; break; }
            skip -= static_cast<uint64_t>(got);
            mUoff += static_cast<uint64_t>(got);
        }

        // 读整行
        if (rowValid) {
            int got = mStream->produce(mRawRow.data(), bytesPerRow);
            if (got < 0) return -1;
            mUoff += static_cast<uint64_t>(got);
            if (got < static_cast<int>(bytesPerRow))
                std::memset(mRawRow.data() + got, 0, bytesPerRow - got);
        } else {
            std::memset(mRawRow.data(), 0, bytesPerRow);
        }
        convertRow(mRawRow.data(), mRowF.data());

        // 行持久化到磁盘 L2 (后续随机访问不再依赖 inflate 游标)
        writeL2Row(r, mRowF.constData());

        // 顺带累积统计 (排除 NODATA=0, Welford)
        {
            const float* p = mRowF.constData();
            for (int c = 0; c < width; ++c) {
                float v = p[c];
                if (v <= 0.0f) continue;
                if (mAccCount == 0) {
                    mAccMin = mAccMax = v;
                } else {
                    if (v < mAccMin) mAccMin = v;
                    if (v > mAccMax) mAccMax = v;
                }
                ++mAccCount;
                double d = v - mAccMean;
                mAccMean += d / mAccCount;
                mAccM2 += d * (v - mAccMean);
            }
        }

        // 输出 wanted 行
        while (wantedIdx < wantedRows.size() && wantedRows[wantedIdx] == r) {
            float* outRow = dst + static_cast<qsizetype>(outIdx) * nOutCols;
            for (int j = 0; j < nOutCols; ++j) {
                int col = x0 + j * xStride;
                outRow[j] = (col < width) ? mRowF[col] : 0.0f;
            }
            ++outIdx;
            ++wantedIdx;
        }

        stageRow(r, mRowF.constData());

        // 抽稀概览 (仅当有需求时)
        if (!mDecimRows.isEmpty() && (r % kDecim) == 0) {
            int d = r / kDecim;
            if (d < mDecimPresent.size() && !mDecimPresent[d]) {
                std::memcpy(mDecimRows.data() + static_cast<qsizetype>(d) * width,
                            mRowF.constData(),
                            static_cast<size_t>(width) * sizeof(float));
                mDecimPresent[d] = 1;
            }
        }
    }

    mInflatedRows = r;
    return outIdx;
}

// ── 缓存拼装 ──
bool TiffStreamDecoder::assembleFromCache(int x0, int y0, int w, int h,
                                          int yStride, int xStride,
                                          float* dst) const
{
    const int width = mTiff->width;
    const int nOutRows = (h + yStride - 1) / yStride;
    const int nOutCols = (w + xStride - 1) / xStride;

    for (int i = 0; i < nOutRows; ++i) {
        int row = y0 + i * yStride;
        int ty = row / kTileSize;
        int ry = row % kTileSize;
        float* outRow = dst + static_cast<qsizetype>(i) * nOutCols;
        for (int j = 0; j < nOutCols; ++j) {
            int col = x0 + j * xStride;
            if (col >= width) { outRow[j] = 0.0f; continue; }
            int tx = col / kTileSize;
            int cx = col % kTileSize;
            auto tile = mCache->get(mCacheKey, tx, ty);
            if (!tile) return false;
            // 部分 tile (取消/中途停止的解码) 行数不足, 视为未命中
            if (ry >= tile->height) return false;
            outRow[j] = tile->data[static_cast<qsizetype>(ry) * tile->width + cx];
        }
    }
    return true;
}

bool TiffStreamDecoder::assembleFromDecim(int x0, int y0, int w, int h,
                                          int yStride, int xStride,
                                          float* dst) const
{
    if (mDecimRows.isEmpty()) return false;
    const int width = mTiff->width;
    const int nOutRows = (h + yStride - 1) / yStride;
    const int nOutCols = (w + xStride - 1) / xStride;

    for (int i = 0; i < nOutRows; ++i) {
        int row = y0 + i * yStride;
        int d = row / kDecim;   // 最近抽稀行 (近似, 概览质量足够)
        if (d >= mDecimPresent.size() || !mDecimPresent[d]) return false;
        const float* srcRow = mDecimRows.constData() + static_cast<qsizetype>(d) * width;
        float* outRow = dst + static_cast<qsizetype>(i) * nOutCols;
        for (int j = 0; j < nOutCols; ++j) {
            int col = x0 + j * xStride;
            outRow[j] = (col < width) ? srcRow[col] : 0.0f;
        }
    }
    return true;
}

// ── 磁盘 L2 行缓存 ──

QString TiffStreamDecoder::l2PathFor(const QString& cacheKey)
{
    QByteArray hash = QCryptographicHash::hash(
        cacheKey.toUtf8(), QCryptographicHash::Md5).toHex();
    return QDir::tempPath() + QStringLiteral("/insar_l2/")
         + QString::fromLatin1(hash) + QStringLiteral(".f32");
}

bool TiffStreamDecoder::ensureL2()
{
    if (mL2Closed || !qEnvironmentVariableIsEmpty("INSAR_L2_DISABLE"))
        return false;
    if (mL2File) return true;
    mL2Path = l2PathFor(mCacheKey);
    QFileInfo fi(mL2Path);
    if (!fi.dir().exists() && !QDir().mkpath(fi.dir().absolutePath())) {
        qWarning() << "[Decoder] L2 dir create failed:" << fi.dir().absolutePath();
        return false;
    }
    // ReadWrite 不截断: 残留的旧缓存文件可在重解码时原位覆盖复用
    auto f = std::make_unique<QFile>(mL2Path);
    if (!f->open(QIODevice::ReadWrite)) {
        qWarning() << "[Decoder] L2 open failed:" << mL2Path;
        return false;
    }
    mL2File = std::move(f);
    mL2Covered.resize(mTiff->height);
    return true;
}

void TiffStreamDecoder::writeL2Row(int row, const float* rowF)
{
    QMutexLocker lock(&mL2Mutex);
    if (!ensureL2() || row < 0 || row >= mTiff->height) return;
    const qint64 rowBytes = static_cast<qint64>(mTiff->width) * 4;
    const qint64 off = static_cast<qint64>(row) * rowBytes;
    if (!mL2File->seek(off)) return;
    if (mL2File->write(reinterpret_cast<const char*>(rowF), rowBytes) == rowBytes)
        mL2Covered[row] = 1;
}

bool TiffStreamDecoder::assembleFromL2(int x0, int y0, int w, int h,
                                       int yStride, int xStride,
                                       float* dst)
{
    QMutexLocker lock(&mL2Mutex);
    if (!ensureL2()) return false;
    const int width = mTiff->width;
    const int nOutRows = (h + yStride - 1) / yStride;
    const int nOutCols = (w + xStride - 1) / xStride;

    if (mL2Covered.size() != mTiff->height) return false;
    for (int i = 0; i < nOutRows; ++i) {
        int row = y0 + i * yStride;
        if (row >= mTiff->height || !mL2Covered[row]) return false;
    }

    const qint64 rowBytes = static_cast<qint64>(width) * 4;
    QVector<float> rowF(width);
    for (int i = 0; i < nOutRows; ++i) {
        int row = y0 + i * yStride;
        if (!mL2File->seek(static_cast<qint64>(row) * rowBytes)) return false;
        if (mL2File->read(reinterpret_cast<char*>(rowF.data()), rowBytes)
            != rowBytes) return false;
        float* outRow = dst + static_cast<qsizetype>(i) * nOutCols;
        for (int j = 0; j < nOutCols; ++j) {
            int col = x0 + j * xStride;
            outRow[j] = (col < width) ? rowF[col] : 0.0f;
        }
    }
    return true;
}

void TiffStreamDecoder::closeL2()
{
    QMutexLocker lock(&mL2Mutex);
    mL2Closed = true;
    if (mL2File) {
        mL2File->close();
        mL2File.reset();
    }
    if (!mL2Path.isEmpty()) {
        QFile::remove(mL2Path);
        mL2Path.clear();
    }
}

// ── 对外接口 ──
int TiffStreamDecoder::readWindow(int x0, int y0, int w, int h,
                                  int yStride, int xStride,
                                  float* dst, QgsRasterBlockFeedback* feedback)
{
    if (!dst || w <= 0 || h <= 0 || yStride <= 0 || xStride <= 0) return 0;
    const int width = mTiff->width;
    const int height = mTiff->height;

    // 裁剪到影像范围
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > width)  w = width - x0;
    if (y0 + h > height) h = height - y0;
    if (w <= 0 || h <= 0) return 0;

    const int nOutRows = (h + yStride - 1) / yStride;
    const int nOutCols = (w + xStride - 1) / xStride;

    // 快速路径 1: 抽稀概览缓存 (中倍缩放直接服务, 避免回卷重新解压)
    if (yStride >= kDecim / 2 && assembleFromDecim(x0, y0, w, h, yStride, xStride, dst))
        return nOutRows;

    // 快速路径 2: tile 缓存
    if (assembleFromCache(x0, y0, w, h, yStride, xStride, dst))
        return nOutRows;

    // 快速路径 3: 磁盘 L2 行缓存
    // (首次完整解码后, 任意缩放/平移的未命中由随机文件读服务,
    //  不再触发回卷重新 inflate)
    if (assembleFromL2(x0, y0, w, h, yStride, xStride, dst))
        return nOutRows;

    // 需要抽稀概览 → 惰性分配
    if (yStride >= kDecim / 2 && mDecimRows.isEmpty()) {
        int nDecim = height / kDecim + 1;
        mDecimRows.resize(static_cast<qsizetype>(nDecim) * width);
        mDecimPresent.resize(nDecim);
        std::memset(mDecimPresent.data(), 0, mDecimPresent.size());
    }

    QVector<int> wantedRows(nOutRows);
    for (int i = 0; i < nOutRows; ++i)
        wantedRows[i] = y0 + i * yStride;

    // 可取消的游标获取: 渲染取消时尽快退出,
    // 避免阻塞 QGIS 的渲染任务收尾 (canvas 刷新会等待任务结束)
    while (!mCursorMutex.tryLock(20)) {
        if (feedback && feedback->isCanceled())
            return 0;
    }

    int out = -1;
    const int maxRow = y0 + h - 1;
    if (mInflatedRows > y0) {
        // 游标已越过请求区 → 回卷
        if (mStream) {
            if (!mStream->restart()) {
                mCursorMutex.unlock();
                return -1;
            }
        } else {
            mStream.reset(mStore->openInflateStream(mEntryName));
            if (!mStream) {
                mCursorMutex.unlock();
                return -1;
            }
        }
        mInflatedRows = 0;
        mUoff = 0;
        mStagingRows = 0;
        mStagingTy = -1;
        // 统计随游标重新累积, 避免重复计数
        mAccCount = 0; mAccMin = 0; mAccMax = 0; mAccMean = 0; mAccM2 = 0;
    }
    out = decodeRows(maxRow, wantedRows, x0, w, xStride, dst, feedback);
    mCursorMutex.unlock();
    return out;
}

bool TiffStreamDecoder::sampledStats(double& min, double& max,
                                     double& mean, double& stdDev,
                                     int maxRows, QgsRasterBlockFeedback* feedback)
{
    Q_UNUSED(maxRows)
    Q_UNUSED(feedback)

    if (mAccCount > 0) {
        // 解码过程中顺带累积的真实统计
        min = mAccMin;
        max = mAccMax;
        mean = mAccMean;
        stdDev = mAccCount > 1 ? std::sqrt(mAccM2 / mAccCount) : 0.0;
        return true;
    }

    // 永不解码 (QGIS 可能在主线程创建渲染器时调用本函数)
    // 回退: annotation 场景统计 (每景自适应), 否则固定理论范围
    if (!sampledPresetStats(min, max, mean, stdDev)) {
        min = 0.0;
        max = 46341.0;   // sqrt(32767² + 32767²)
        mean = 5000.0;
        stdDev = 6000.0;
    }
    return true;
}

void TiffStreamDecoder::ensureStats()
{
    if (mAccCount > 0) return;
    const int height = mTiff->height;
    const int width = mTiff->width;
    int stride = std::max(1, height / 256);
    const int chunk = 2048;
    QVector<float> buf(static_cast<qsizetype>(chunk / stride + 2) * width);
    for (int y = 0; y < height; y += chunk) {
        if (mAccCount > 0) break;   // 预热任务已抢先完成
        int hh = std::min(chunk, height - y);
        readWindow(0, y, width, hh, stride, 1, buf.data(), nullptr);
    }
}

bool TiffStreamDecoder::sampledPresetStats(double& min, double& max,
                                           double& mean, double& stdDev) const
{
    if (mTiff->presetMax > 0) {
        min = 0.0;
        max = mTiff->presetMax;
        mean = mTiff->presetMean;
        stdDev = mTiff->presetStd;
        return true;
    }
    return false;
}
