#include "GdalVsiProcessor.h"

#include <gdal_priv.h>
#include <cmath>
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
    // 1. 生成临时 VRT —— GDAL 内部处理 CInt16→振幅 的像素函数
    QString vrtPath = tifPath + ".tmp.vrt";
    if (!createAmplitudeVRT(vsiPath, vrtPath))
        return false;

    // 2. 打开 VRT, 写出为 TIFF —— GDAL 内部逐块执行像素函数
    GDALDatasetH vrtDS = GDALOpen(vrtPath.toUtf8().constData(), GA_ReadOnly);
    if (!vrtDS) { QFile::remove(vrtPath); return false; }

    GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
    GDALDatasetH dstDS = nullptr;
    if (gtDrv) {
        dstDS = GDALCreateCopy(gtDrv, tifPath.toUtf8().constData(),
                                vrtDS, FALSE, nullptr, nullptr, nullptr);
    }
    GDALClose(vrtDS);
    QFile::remove(vrtPath);

    if (!dstDS) return false;

    // 3. 构建金字塔用于多分辨率渲染
    int levels[] = {2, 4, 8, 16, 32, 64};
    GDALBuildOverviews(dstDS, "AVERAGE", 6, levels, 0, nullptr, nullptr,
                       nullptr);
    GDALClose(dstDS);
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
