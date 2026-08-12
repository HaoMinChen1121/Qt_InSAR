#ifndef ZIPSTORE_H
#define ZIPSTORE_H

#include <QString>
#include <QStringList>
#include <QMap>
#include <QMutex>
#include <QFile>
#include <vector>
#include <memory>
#include <cstdint>

// ── zlib 动态加载 (进程级共享, 供 ZipStore / 其他读取器使用) ──

// 与 zlib 1.2.x z_stream 布局一致 (Win64)
struct ZlibStream {
    const uint8_t* next_in  = nullptr;
    uint32_t       avail_in  = 0;
    unsigned long  total_in  = 0;
    uint8_t*       next_out  = nullptr;
    uint32_t       avail_out = 0;
    unsigned long  total_out = 0;
    const char*    msg       = nullptr;
    void*          state     = nullptr;
    void*          zalloc    = nullptr;
    void*          zfree     = nullptr;
    void*          opaque    = nullptr;
    int            data_type = 0;
    unsigned long  adler     = 0;
    unsigned long  reserved  = 0;
};

namespace zlibshared {
// 返回 false 表示 zlib 不可用
bool ensureLoaded();
int inflateInit2(ZlibStream* s, int windowBits, const char* version, int streamSize);
int inflate(ZlibStream* s, int flush);
int inflateEnd(ZlibStream* s);
int inflateReset(ZlibStream* s);
}

struct ZipEntryInfo {
    QString  name;                 // ZIP 内原始文件名
    uint64_t dataOffset = 0;       // 压缩数据起始偏移
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint16_t method = 0;           // 0=stored, 8=deflate
};

class ZipInflateStream;

// 持久 ZIP 读取器: 打开一次, 中央目录解析一次, 供多次 entry 读取/流式解压
class ZipStore {
public:
    // 按路径进程级共享 (缓存弱引用, 无引用时自动释放)
    static std::shared_ptr<ZipStore> open(const QString& zipPath);

    const ZipEntryInfo* findEntry(const QString& name) const;   // 精确 → 忽略大小写回退
    QStringList entryList() const;
    QString zipPath() const { return mZipPath; }

    // 整读一个 entry (小文件: annotation XML / TIFF 头)
    std::vector<unsigned char> readEntry(const QString& entryName);
    std::vector<unsigned char> readEntry(const ZipEntryInfo& e);

    // 打开顺序 inflate 流 (每个流使用独立文件句柄, 线程安全)
    ZipInflateStream* openInflateStream(const QString& entryName);
    ZipInflateStream* openInflateStream(const ZipEntryInfo& e);

private:
    ZipStore() = default;
    bool load(const QString& zipPath);

    QString mZipPath;
    QMap<QString, ZipEntryInfo> mEntries;   // key: 小写文件名
};

// 可暂停/恢复的顺序 inflate 游标 (对应"持续解压状态")
class ZipInflateStream {
public:
    // 拉取式: 持续 inflate 直到产出 wantBytes 或流结束
    // 返回实际产出字节数; <0 表示错误
    int produce(uint8_t* dst, size_t wantBytes);
    bool atEnd() const { return mAtEnd; }
    // 回卷: 重置 inflate 状态并回到 entry 压缩数据起始
    bool restart();

    // 析构必须 public: std::unique_ptr 默认删除器需要访问
    ~ZipInflateStream();

private:
    friend class ZipStore;
    ZipInflateStream(const QString& zipPath, const ZipEntryInfo& entry);

    bool feedInput();               // 从文件读下一块压缩数据

    QFile mFile;
    uint64_t mDataOffset = 0;
    uint64_t mCompressedSize = 0;
    uint64_t mRemaining = 0;        // 剩余未读压缩字节
    uint64_t mPos = 0;              // stored entry 已读位置
    uint16_t mMethod = 0;
    ZlibStream* mZStream = nullptr; // zlib 流状态
    std::vector<uint8_t> mInBuf;    // 压缩数据块缓冲
    bool mAtEnd = false;
    bool mStarted = false;
};

#endif // ZIPSTORE_H
