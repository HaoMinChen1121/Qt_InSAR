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

    // 打开 ZIP 内的 Sentinel-1 SLC TIFF
    // zipPath:  ZIP 文件系统路径
    // entryName: ZIP 内 TIFF 路径 (如 "S1A_xxx.SAFE/measurement/iw1-vv.tiff")
    bool open(const QString& zipPath, const QString& entryName,
              const SarBandDescriptor& band);
    void close();
    bool isOpen() const { return !mCaches.empty() || mVsiDataset != nullptr; }

    int width() const { return mWidth; }
    int height() const { return mHeight; }

    int burstCount() const { return mBurstCount; }
    int linesPerBurst() const { return mLinesPerBurst; }
    int burstStartLine(int idx) const { return mBurstStartLines.value(idx, -1); }
    double azimuthFmRate() const { return mFmRate; }
    double azimuthSteeringRate() const { return mSteeringRate; }
    double azimuthFrequency() const { return mPrf; }

    sar::ComplexSoAView burstSoaView(int idx);
    bool readWindow(int x0, int y0, int w, int h, std::complex<float>* dst);

    void* datasetHandle() const { return mVsiDataset; }

private:
    QString mMemPath;               // /vsimem/ 路径 (用于清理)
    void* mVsiDataset = nullptr;    // GDAL dataset (仅用于地理参考)
    int mWidth = 0;
    int mHeight = 0;
    int mBurstCount = 0;
    int mLinesPerBurst = 0;
    double mFmRate = 0.0;
    double mSteeringRate = 0.0;
    double mPrf = 0.0;

    QVector<int> mBurstStartLines;
    std::vector<BurstCacheSoA> mCaches;
    std::vector<unsigned char> mRawTiff;  // TIFF 完整字节 (ZIP解压后)
};

#endif // SENTINELDATAREADER_H
