#include "SentinelDataReader.h"
#include "domain/SarComplexTypes.h"
#include <gdal_priv.h>
#include <cpl_vsi.h>
#include <cpl_conv.h>
#include <QDebug>
#include <vector>

SentinelDataReader::~SentinelDataReader() { close(); }

bool SentinelDataReader::open(const QString& vsiPath, const SarBandDescriptor& band)
{
    close();

    // ═══════════════════════════════════════════════
    //  Step 1: 将 ZIP 内 TIFF 一次性读入内存 (单次顺
    //  序 VSI 读取, DEFLATE 仅解压一次, 消除 per-strip
    //  VSI 调用开销)
    // ═══════════════════════════════════════════════
    VSILFILE* fp = VSIFOpenExL(vsiPath.toUtf8().constData(), "rb", TRUE);
    if (!fp) {
        qWarning() << "[SDR] Cannot open VSI file:" << vsiPath;
        return false;
    }

    VSIStatBufL st;
    if (VSIStatL(vsiPath.toUtf8().constData(), &st) != 0 || st.st_size <= 0) {
        qWarning() << "[SDR] Cannot stat VSI file:" << vsiPath;
        VSIFCloseL(fp);
        return false;
    }

    mRawTiff.resize(static_cast<size_t>(st.st_size));
    vsi_l_offset totalRead = 0;
    vsi_l_offset remaining = static_cast<vsi_l_offset>(st.st_size);
    while (remaining > 0) {
        vsi_l_offset n = VSIFReadL(mRawTiff.data() + totalRead, 1, remaining, fp);
        if (n == 0) break;
        totalRead += n;
        remaining -= n;
    }
    VSIFCloseL(fp);

    if (totalRead < static_cast<vsi_l_offset>(st.st_size)) {
        qWarning() << "[SDR] Incomplete VSI read" << totalRead << "/" << st.st_size;
        return false;
    }

    // ═══════════════════════════════════════════════
    //  Step 2: 创建 /vsimem/ 文件 (mRawTiff 作为成员
    //  保持存活), GDAL 后续所有 I/O 走 RAM
    // ═══════════════════════════════════════════════
    mMemPath = QString("/vsimem/_sdr_%1.tif").arg(
        reinterpret_cast<quintptr>(this), 0, 16);
    VSIFCloseL(VSIFileFromMemBuffer(mMemPath.toUtf8().constData(),
        mRawTiff.data(), st.st_size, FALSE));

    mVsiDataset = GDALOpenShared(mMemPath.toUtf8().constData(), GA_ReadOnly);
    if (!mVsiDataset) {
        qWarning() << "[SDR] Failed to open /vsimem/ TIFF";
        VSIUnlink(mMemPath.toUtf8().constData());
        mMemPath.clear();
        return false;
    }

    mWidth  = GDALGetRasterXSize(static_cast<GDALDatasetH>(mVsiDataset));
    mHeight = GDALGetRasterYSize(static_cast<GDALDatasetH>(mVsiDataset));

    mBurstCount    = band.burstCount;
    mLinesPerBurst = band.linesPerBurst;
    mBurstStartLines = band.burstStartLines;
    mFmRate        = band.azimuthFmRate;
    mSteeringRate  = band.azimuthSteeringRate;
    mPrf           = band.azimuthFrequency;

    if (mBurstCount <= 0) return true;

    // ═══════════════════════════════════════════════
    //  Step 3: 从 /vsimem/ 全图 RasterIO → SoA burst 切分
    //  (此时 strips 已在 RAM 中, 无 VSI 开销)
    // ═══════════════════════════════════════════════
    std::vector<CFloat32> fullBuf(static_cast<size_t>(mWidth) * mHeight);
    GDALDataset* ds = static_cast<GDALDataset*>(mVsiDataset);
    GDALRasterBand* rb = ds->GetRasterBand(1);
    CPLErr err = rb->RasterIO(GF_Read, 0, 0, mWidth, mHeight,
        fullBuf.data(), mWidth, mHeight, GDT_CFloat32, 0, 0);
    if (err != CE_None) {
        qWarning() << "[SDR] Full-image RasterIO failed";
        close();
        return false;
    }

    // ═══════════════════════════════════════════════
    //  Step 4: 切分为各 burst 并 deramp
    // ═══════════════════════════════════════════════
    mCaches.resize(mBurstCount);
    for (int b = 0; b < mBurstCount; ++b) {
        int row0 = mBurstStartLines.value(b, b * mLinesPerBurst);
        int burstH = mLinesPerBurst;
        if (b == mBurstCount - 1)
            burstH = mHeight - row0;
        if (burstH <= 0 || row0 < 0 || row0 + burstH > mHeight) {
            qWarning() << "[SDR] Invalid burst bounds" << b << "row0=" << row0
                       << "burstH=" << burstH;
            continue;
        }

        mCaches[b].loadFromCfloat32(
            fullBuf.data() + static_cast<size_t>(row0) * mWidth,
            mWidth, burstH);

        if (mPrf > 0.0 && std::abs(mFmRate) > 1e-6)
            mCaches[b].applyDeramp(mPrf, mFmRate, row0, b);
    }

    qDebug() << "[SDR] Preloaded" << mBurstCount << "bursts" << mWidth << "x" << mHeight;
    return true;
}

void SentinelDataReader::close()
{
    mCaches.clear();
    if (mVsiDataset) {
        GDALClose(static_cast<GDALDatasetH>(mVsiDataset));
        mVsiDataset = nullptr;
    }
    if (!mMemPath.isEmpty()) {
        VSIUnlink(mMemPath.toUtf8().constData());
        mMemPath.clear();
    }
    mWidth = mHeight = 0;
    mBurstCount = mLinesPerBurst = 0;
    mBurstStartLines.clear();
    mVsiPath.clear();
    mRawTiff.clear();
    mRawTiff.shrink_to_fit();
}

sar::ComplexSoAView SentinelDataReader::burstSoaView(int idx)
{
    sar::ComplexSoAView empty{};
    if (idx < 0 || idx >= mBurstCount) return empty;

    if (!mCaches[idx].isLoaded()) return empty;
    return mCaches[idx].soaView();
}

bool SentinelDataReader::readWindow(int x0, int y0, int w, int h, std::complex<float>* dst)
{
    if (!dst || w <= 0 || h <= 0) return false;
    if (x0 < 0 || y0 < 0 || x0 + w > mWidth || y0 + h > mHeight) return false;
    if (mLinesPerBurst <= 0 || mBurstCount <= 0) return false;

    // 初始化为零 (burst 间隙填充)
    std::fill(dst, dst + static_cast<size_t>(w) * h, std::complex<float>(0.0f, 0.0f));

    // 确定窗口跨越的 burst 范围
    int burstFirst = y0 / mLinesPerBurst;
    int burstLast  = (y0 + h - 1) / mLinesPerBurst;
    if (burstFirst < 0) burstFirst = 0;
    if (burstLast >= mBurstCount) burstLast = mBurstCount - 1;

    for (int bi = burstFirst; bi <= burstLast; ++bi) {
        if (!mCaches[bi].isLoaded()) continue;

        int burstRow0 = mBurstStartLines.value(bi, bi * mLinesPerBurst);
        int burstH    = (bi == mBurstCount - 1)
                        ? mHeight - burstRow0 : mLinesPerBurst;

        int interY0 = std::max(y0, burstRow0);
        int interY1 = std::min(y0 + h, burstRow0 + burstH);
        if (interY0 >= interY1) continue;

        int srcX = x0;
        int srcY = interY0 - burstRow0;
        int copyW = w;
        int copyH = interY1 - interY0;
        int dstOff = (interY0 - y0) * w;

        mCaches[bi].getWindow(srcX, srcY, copyW, copyH, dst + dstOff);
    }
    return true;
}
