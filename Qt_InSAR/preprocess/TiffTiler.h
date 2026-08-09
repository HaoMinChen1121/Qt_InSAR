#ifndef TIFFTILER_H
#define TIFFTILER_H

#include <QString>

// ── Strip TIFF → 256×256 Tiled CInt16 TIFF 转换 ──
// 解决 Sentinel-1 SLC Strip 组织 (Block=24561×1) 导致的
// GDAL 随机读放大问题: 128×128 窗口实际读 12MB。
// 转为 Tiled 后同等窗口只需读 ~256KB。

class TiffTiler {
public:
    // 确保 tiled 缓存存在, 返回 tiled TIFF 路径。
    // slcPath: 原始 SLC TIFF 路径 (本地文件或 /vsizip/...)
    // 若已缓存则直接返回; 否则 GDALCreateCopy 到 cacheDir。
    static QString ensureTiled(const QString& slcPath,
                               const QString& cacheDir);

    // 清理所有缓存
    static void cleanupCache(const QString& cacheDir);

private:
    TiffTiler() = delete;
};

#endif // TIFFTILER_H
