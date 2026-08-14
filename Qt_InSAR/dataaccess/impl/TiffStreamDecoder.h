#ifndef TIFFSTREAMDECODER_H
#define TIFFSTREAMDECODER_H

#include "ZipStore.h"
#include "ZipTiffExtractor.h"
#include "renderers/TileCache.h"
#include <QString>
#include <QMutex>
#include <QVector>
#include <QFile>
#include <memory>
#include <cstdint>

class QgsRasterBlockFeedback;

// 顺序 TIFF 解码器: 持有 inflate 游标 (只能前进, 回退=从头 restart),
// 逐行转 Float32 振幅/实值, 填充 Tile 缓存与 8 行抽稀概览缓存。
// 每 (zipPath, entry) 一个实例; 游标访问由内部 mutex 串行化。
class TiffStreamDecoder {
public:
    TiffStreamDecoder(std::shared_ptr<ZipStore> store,
                      const QString& entryName,
                      std::shared_ptr<const TiffHeaderInfo> tiff,
                      std::shared_ptr<TileCache> cache);

    int width() const { return mTiff->width; }
    int height() const { return mTiff->height; }

    // 读取窗口 (输出抽稀): dst 大小 = nOutRows * nOutCols
    //   nOutRows = ceil(h / yStride), nOutCols = ceil(w / xStride)
    // 返回实际输出行数; <0 表示失败
    int readWindow(int x0, int y0, int w, int h,
                   int yStride, int xStride,
                   float* dst, QgsRasterBlockFeedback* feedback);

    // 统计 (排除 NODATA=0 像素) — 永不解码 (QGIS 可能在主线程调用!)
    // 无累积数据时返回 annotation 场景统计/固定预设
    bool sampledStats(double& min, double& max, double& mean, double& stdDev,
                      int maxRows, QgsRasterBlockFeedback* feedback);

    // 后台任务专用: 分块解码直至统计累积 (供异步拉伸更新)
    void ensureStats();

    // annotation 场景统计预设 (不解码, 供初始拉伸占位)
    bool sampledPresetStats(double& min, double& max,
                            double& mean, double& stdDev) const;

    // 关闭并删除磁盘 L2 行缓存 (产品销毁时调用)
    void closeL2();

private:
    // 从 tile 缓存拼装 (全部命中才成功)
    bool assembleFromCache(int x0, int y0, int w, int h,
                           int yStride, int xStride, float* dst) const;
    // 从抽稀概览缓存拼装 (仅当全图抽稀已收集)
    bool assembleFromDecim(int x0, int y0, int w, int h,
                           int yStride, int xStride, float* dst) const;
    // 从磁盘 L2 行缓存拼装 (全部行已解码入库才成功)
    bool assembleFromL2(int x0, int y0, int w, int h,
                        int yStride, int xStride, float* dst);
    // 惰性创建 L2 文件 (调用前须持有 mL2Mutex; mClosed 后不再创建)
    bool ensureL2();
    // 解码行写入 L2 (内部加锁)
    void writeL2Row(int row, const float* rowF);
    // L2 临时文件路径 (zipPath|entry 的 MD5 命名, 跨会话可复用)
    static QString l2PathFor(const QString& cacheKey);
    // 顺序 inflate 到 maxRow (含), 途中输出 wanted 行
    // 调用前必须持有 mCursorMutex
    int decodeRows(int maxRow,
                   const QVector<int>& wantedRows,
                   int x0, int w, int xStride,
                   float* dst, QgsRasterBlockFeedback* feedback);
    // 单行原始字节 → 全宽 Float32
    void convertRow(const uint8_t* raw, float* rowF);
    // 行写入 tile 暂存带 (256 行满即拆分为 x-tile 入缓存)
    void stageRow(int row, const float* rowF);
    void flushStaging();

    std::shared_ptr<ZipStore> mStore;
    QString mEntryName;
    std::shared_ptr<const TiffHeaderInfo> mTiff;
    std::shared_ptr<TileCache> mCache;
    QString mCacheKey;

    // ── inflate 游标 (mCursorMutex 保护) ──
    QMutex mCursorMutex;
    std::unique_ptr<ZipInflateStream> mStream;
    int mInflatedRows = 0;          // 下一个待解码行号 (0 起始)
    uint64_t mUoff = 0;             // 解压流内已消费的未压缩偏移

    // 行缓冲
    std::vector<uint8_t> mRawRow;   // bytesPerRow
    std::vector<uint8_t> mSkipBuf;  // strip 间隙跳过缓冲
    QVector<float> mRowF;           // 全宽 Float32 行

    // tile 暂存带: 256 行 x 全宽
    static constexpr int kTileSize = TileCache::kTileSize;
    QVector<float> mStaging;
    int mStagingRows = 0;
    int mStagingTy = -1;

    // 抽稀概览行缓存: 每 kDecim 行一行
    static constexpr int kDecim = 8;
    QVector<float> mDecimRows;
    QVector<uint8_t> mDecimPresent; // 位图

    // ── 磁盘 L2 行缓存: 解码行持久化, 支持随机访问 ──
    // 行 r 位于文件偏移 r*width*4; mL2Covered 标记已入库行
    // (QFile 非线程安全: 所有文件操作 + 覆盖位图由 mL2Mutex 串行化)
    QMutex mL2Mutex;
    std::unique_ptr<QFile> mL2File;
    QString mL2Path;
    QVector<uint8_t> mL2Covered;
    bool mL2Closed = false;

    // 统计累积 (解码时顺带计算, Welford; 游标重启时清零)
    uint64_t mAccCount = 0;
    double mAccMin = 0, mAccMax = 0, mAccMean = 0, mAccM2 = 0;
};

#endif // TIFFSTREAMDECODER_H
