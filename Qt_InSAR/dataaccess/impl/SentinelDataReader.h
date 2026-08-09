#ifndef SENTINELDATAREADER_H
#define SENTINELDATAREADER_H

#include "BurstCacheSoA.h"
#include "dataaccess/ISarProduct.h"
#include <QString>
#include <QVector>
#include <vector>
#include <complex>

class SentinelDataReader {
public:
    SentinelDataReader() = default;
    ~SentinelDataReader();

    SentinelDataReader(const SentinelDataReader&) = delete;
    SentinelDataReader& operator=(const SentinelDataReader&) = delete;

    // 打开 Sentinel-1 SLC TIFF (通过 GDAL VSI 路径)
    bool open(const QString& vsiPath, const SarBandDescriptor& band);
    void close();
    bool isOpen() const { return mVsiDataset != nullptr; }

    // 图像尺寸
    int width() const { return mWidth; }
    int height() const { return mHeight; }

    // Burst 元数据
    int burstCount() const { return mBurstCount; }
    int linesPerBurst() const { return mLinesPerBurst; }
    int burstStartLine(int idx) const { return mBurstStartLines.value(idx, -1); }
    double azimuthFmRate() const { return mFmRate; }
    double azimuthSteeringRate() const { return mSteeringRate; }
    double azimuthFrequency() const { return mPrf; }

    // 延迟加载并返回零拷贝 SoA 视图 (SincResampler 用)
    sar::ComplexSoAView burstSoaView(int idx);

    // 从缓存读取矩形窗口 (correlator/ESD 用)
    bool readWindow(int x0, int y0, int w, int h, std::complex<float>* dst);

    // 数据集句柄 (给 GdalSlcWriter 复制地理参考)
    void* datasetHandle() const { return mVsiDataset; }

private:
    QString mVsiPath;
    QString mMemPath;               // /vsimem/ 路径 (用于清理)
    void* mVsiDataset = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    int mBurstCount = 0;
    int mLinesPerBurst = 0;
    double mFmRate = 0.0;
    double mSteeringRate = 0.0;
    double mPrf = 0.0;

    QVector<int> mBurstStartLines;
    std::vector<BurstCacheSoA> mCaches;
    std::vector<unsigned char> mRawTiff;  // /vsimem/ backing buffer
};

#endif // SENTINELDATAREADER_H
