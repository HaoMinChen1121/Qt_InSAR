#include "GdalVsiProcessor.h"

#include <gdal_priv.h>
#include <cmath>
#include <vector>
#include <QFile>
#include <QTextStream>
#include <QDebug>

// ---- file-scope: GDAL pixel function for complex-to-amplitude conversion ----

static CPLErr amplitudePixelFunc(void **papoSources, int nSources,
                                  void *pData, int nXSize, int nYSize,
                                  GDALDataType eSrcType,
                                  GDALDataType /* eBufType */,
                                  int nPixelSpace, int nLineSpace)
{
    if (nSources != 1 || !papoSources[0] || !pData)
        return CE_Failure;

    if (eSrcType == GDT_CInt16) {
        const auto* src = static_cast<const int16_t*>(papoSources[0]);
        auto* dst = static_cast<GByte*>(pData);

        for (int iy = 0; iy < nYSize; iy++) {
            for (int ix = 0; ix < nXSize; ix++) {
                int si = (iy * nXSize + ix) * 2;
                float I = static_cast<float>(src[si]);
                float Q = static_cast<float>(src[si + 1]);
                *reinterpret_cast<float*>(dst + iy * nLineSpace
                                          + ix * nPixelSpace) =
                    std::sqrt(I * I + Q * Q);
            }
        }
        return CE_None;
    }

    if (eSrcType == GDT_CFloat32) {
        const auto* src = static_cast<const float*>(papoSources[0]);
        auto* dst = static_cast<GByte*>(pData);

        for (int iy = 0; iy < nYSize; iy++) {
            for (int ix = 0; ix < nXSize; ix++) {
                int si = (iy * nXSize + ix) * 2;
                float I = src[si];
                float Q = src[si + 1];
                *reinterpret_cast<float*>(dst + iy * nLineSpace
                                          + ix * nPixelSpace) =
                    std::sqrt(I * I + Q * Q);
            }
        }
        return CE_None;
    }

    return CE_Failure;
}

// ---- file-scope: VRT XML generation for on-the-fly amplitude rendering ----

static bool createAmplitudeVRT(const QString& vsiPath, const QString& vrtPath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return false;

    int w = GDALGetRasterXSize(srcDS);
    int h = GDALGetRasterYSize(srcDS);
    GDALRasterBandH srcBand = GDALGetRasterBand(srcDS, 1);
    QString srcTypeName = QString::fromUtf8(
        GDALGetDataTypeName(GDALGetRasterDataType(srcBand)));

    QFile f(vrtPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        GDALClose(srcDS);
        return false;
    }

    QTextStream ts(&f);
    ts << "<VRTDataset rasterXSize=\"" << w
       << "\" rasterYSize=\"" << h << "\">\n";

    const char* proj = GDALGetProjectionRef(srcDS);
    if (proj && strlen(proj) > 0)
        ts << "  <SRS><![CDATA[" << proj << "]]></SRS>\n";

    int nGCPs = GDALGetGCPCount(srcDS);
    if (nGCPs > 0) {
        const char* gcpProj = GDALGetGCPProjection(srcDS);
        QString escapedProj = QString::fromUtf8(gcpProj ? gcpProj : "");
        escapedProj.replace("&", "&amp;")
                   .replace("\"", "&quot;")
                   .replace("<", "&lt;")
                   .replace(">", "&gt;");
        ts << "  <GCPList projection=\""
           << escapedProj << "\">\n";
        const GDAL_GCP* gcps = GDALGetGCPs(srcDS);
        for (int i = 0; i < nGCPs; ++i) {
            ts << "    <GCP Id=\"" << (i + 1)
               << "\" Pixel=\"" << gcps[i].dfGCPPixel
               << "\" Line=\"" << gcps[i].dfGCPLine
               << "\" X=\"" << gcps[i].dfGCPX
               << "\" Y=\"" << gcps[i].dfGCPY
               << "\" Z=\"" << gcps[i].dfGCPZ << "\"/>\n";
        }
        ts << "  </GCPList>\n";
    } else {
        double gt[6];
        if (GDALGetGeoTransform(srcDS, gt) == CE_None) {
            ts << "  <GeoTransform>" << gt[0] << ", " << gt[1] << ", "
               << gt[2] << ", " << gt[3] << ", " << gt[4] << ", "
               << gt[5] << "</GeoTransform>\n";
        }
    }

    ts << "  <VRTRasterBand dataType=\"Float32\" band=\"1\""
          " subClass=\"VRTDerivedRasterBand\">\n";
    ts << "    <PixelFunctionType>amplitude</PixelFunctionType>\n";
    ts << "    <SimpleSource>\n";
    ts << "      <SourceFilename>" << vsiPath << "</SourceFilename>\n";
    ts << "      <SourceBand>1</SourceBand>\n";
    ts << "      <SrcDataType>" << srcTypeName << "</SrcDataType>\n";
    ts << "    </SimpleSource>\n";
    // 预置近似统计值，避免 QGIS 全图扫描触发 /vsizip 解压风暴
    // CInt16 振幅理论范围 0 ~ sqrt(32767²+32767²) ≈ 46341
    ts << "    <Metadata>\n";
    ts << "      <MDI key=\"STATISTICS_MINIMUM\">0</MDI>\n";
    ts << "      <MDI key=\"STATISTICS_MAXIMUM\">46341</MDI>\n";
    ts << "      <MDI key=\"STATISTICS_MEAN\">5000</MDI>\n";
    ts << "      <MDI key=\"STATISTICS_STDDEV\">6000</MDI>\n";
    ts << "    </Metadata>\n";
    ts << "  </VRTRasterBand>\n";
    ts << "</VRTDataset>\n";

    GDALClose(srcDS);
    f.close();
    return true;
}

// ---- file-scope: pre-computed amplitude GeoTIFF for SLC (complex) data ----

static bool createAmplitudeGeoTiff(const QString& vsiPath,
                                    const QString& tifPath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return false;

    int w = GDALGetRasterXSize(srcDS);
    int h = GDALGetRasterYSize(srcDS);
    GDALRasterBandH srcBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcDT = GDALGetRasterDataType(srcBand);

    GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
    if (!gtDrv) { GDALClose(srcDS); return false; }

    GDALDatasetH dstDS = GDALCreate(gtDrv, tifPath.toUtf8().constData(),
                                     w, h, 1, GDT_Float32, nullptr);
    if (!dstDS) { GDALClose(srcDS); return false; }

    // Copy georeferencing
    double gt[6];
    if (GDALGetGeoTransform(srcDS, gt) == CE_None)
        GDALSetGeoTransform(dstDS, gt);
    const char* proj = GDALGetProjectionRef(srcDS);
    if (proj && strlen(proj) > 0)
        GDALSetProjection(dstDS, proj);

    int nGCPs = GDALGetGCPCount(srcDS);
    if (nGCPs > 0) {
        std::vector<GDAL_GCP> gcps(nGCPs);
        const GDAL_GCP* srcGcps = GDALGetGCPs(srcDS);
        for (int i = 0; i < nGCPs; ++i)
            gcps[i] = srcGcps[i];
        GDALSetGCPs(dstDS, nGCPs, gcps.data(),
                     GDALGetGCPProjection(srcDS));
    }

    GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);

    int blockXSize, blockYSize;
    GDALGetBlockSize(srcBand, &blockXSize, &blockYSize);
    if (blockXSize <= 0 || blockYSize <= 0) {
        blockXSize = w;
        blockYSize = 1;
    }

    if (srcDT == GDT_CInt16) {
        std::vector<int16_t> srcBuf(blockXSize * blockYSize * 2);
        std::vector<float> dstBuf(blockXSize * blockYSize);

        for (int y = 0; y < h; y += blockYSize) {
            int rows = (y + blockYSize <= h) ? blockYSize : (h - y);
            if (GDALRasterIO(srcBand, GF_Read, 0, y, w, rows,
                             srcBuf.data(), w, rows, GDT_CInt16, 0, 0)
                != CE_None) break;

            int n = w * rows;
            for (int i = 0; i < n; i++) {
                float I = static_cast<float>(srcBuf[i * 2]);
                float Q = static_cast<float>(srcBuf[i * 2 + 1]);
                dstBuf[i] = std::sqrt(I * I + Q * Q);
            }

            GDALRasterIO(dstBand, GF_Write, 0, y, w, rows,
                         dstBuf.data(), w, rows, GDT_Float32, 0, 0);
        }
    } else if (srcDT == GDT_CFloat32) {
        std::vector<float> srcBuf(blockXSize * blockYSize * 2);
        std::vector<float> dstBuf(blockXSize * blockYSize);

        for (int y = 0; y < h; y += blockYSize) {
            int rows = (y + blockYSize <= h) ? blockYSize : (h - y);
            if (GDALRasterIO(srcBand, GF_Read, 0, y, w, rows,
                             srcBuf.data(), w, rows, GDT_CFloat32, 0, 0)
                != CE_None) break;

            int n = w * rows;
            for (int i = 0; i < n; i++) {
                float I = srcBuf[i * 2];
                float Q = srcBuf[i * 2 + 1];
                dstBuf[i] = std::sqrt(I * I + Q * Q);
            }

            GDALRasterIO(dstBand, GF_Write, 0, y, w, rows,
                         dstBuf.data(), w, rows, GDT_Float32, 0, 0);
        }
    } else {
        GDALClose(dstDS);
        GDALClose(srcDS);
        return false;
    }

    int levels[] = {2, 4, 8, 16, 32, 64};
    GDALBuildOverviews(dstDS, "AVERAGE", 6, levels, 0, nullptr, nullptr,
                       nullptr);

    GDALClose(dstDS);
    GDALClose(srcDS);
    return true;
}

// ---- public static methods ----

void GdalVsiProcessor::registerPixelFunctions()
{
    GDALAddDerivedBandPixelFunc("amplitude", amplitudePixelFunc);
}

QString GdalVsiProcessor::process(const QString& vsiPath,
                                   const QString& outputBasePath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return QString();

    GDALRasterBandH hBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcType = GDALGetRasterDataType(hBand);
    QString result;

    if (GDALDataTypeIsComplex(srcType)) {
        GDALClose(srcDS);
        srcDS = nullptr;

        QString tifPath = outputBasePath + ".tif";
        if (createAmplitudeGeoTiff(vsiPath, tifPath))
            result = tifPath;
    } else {
        QString tifPath = outputBasePath + ".tif";
        GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
        if (gtDrv) {
            GDALDatasetH dstDS = GDALCreateCopy(gtDrv,
                tifPath.toUtf8().constData(), srcDS, FALSE,
                nullptr, nullptr, nullptr);
            if (dstDS) {
                int levels[] = {2, 4, 8, 16, 32, 64};
                GDALBuildOverviews(dstDS, "NEAREST", 6,
                    levels, 0, nullptr, nullptr, nullptr);
                GDALClose(dstDS);
                result = tifPath;
            }
        }
    }
    if (srcDS)
        GDALClose(srcDS);
    return result;
}
