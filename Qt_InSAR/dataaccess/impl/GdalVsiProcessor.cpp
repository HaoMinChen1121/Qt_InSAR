#include "GdalVsiProcessor.h"
#include "ZipTiffExtractor.h"

#include <gdal_priv.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDebug>
#include "domain/SarComplexTypes.h"

// ---- GDAL pixel function (still needed for VRT rendering by QGIS) ----

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
                *reinterpret_cast<float*>(dst + iy * nLineSpace + ix * nPixelSpace) =
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
                float I = src[si]; float Q = src[si + 1];
                *reinterpret_cast<float*>(dst + iy * nLineSpace + ix * nPixelSpace) =
                    std::sqrt(I * I + Q * Q);
            }
        }
        return CE_None;
    }
    return CE_Failure;
}

// ---- VRT XML generation (zero GDAL dependency) ----

static bool createAmplitudeVRT(const QString& sourceTiffPath, const QString& vrtPath,
                                int w, int h, const TiffHeaderInfo& info)
{
    QFile f(vrtPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;

    QTextStream ts(&f);
    ts << "<VRTDataset rasterXSize=\"" << w << "\" rasterYSize=\"" << h << "\">\n";

    // GCP list
    if (!info.gcps.isEmpty()) {
        if (!info.projectionWkt.isEmpty()) {
            QString esc = info.projectionWkt;
            esc.replace("&", "&amp;").replace("\"", "&quot;")
               .replace("<", "&lt;").replace(">", "&gt;");
            ts << "  <GCPList projection=\"" << esc << "\">\n";
        } else {
            ts << "  <GCPList>\n";
        }
        for (int i = 0; i < info.gcps.size(); ++i) {
            const auto& g = info.gcps[i];
            ts << "    <GCP Id=\"" << (i + 1)
               << "\" Pixel=\"" << g.pixel
               << "\" Line=\"" << g.line
               << "\" X=\"" << g.lon
               << "\" Y=\"" << g.lat
               << "\" Z=\"" << g.height << "\"/>\n";
        }
        ts << "  </GCPList>\n";
    }

    ts << "  <VRTRasterBand dataType=\"Float32\" band=\"1\""
          " subClass=\"VRTDerivedRasterBand\">\n";
    ts << "    <PixelFunctionType>amplitude</PixelFunctionType>\n";
    ts << "    <SimpleSource>\n";
    ts << "      <SourceFilename>" << sourceTiffPath << "</SourceFilename>\n";
    ts << "      <SourceBand>1</SourceBand>\n";
    ts << "      <SrcDataType>CInt16</SrcDataType>\n";
    ts << "    </SimpleSource>\n";
    ts << "  </VRTRasterBand>\n";
    ts << "</VRTDataset>\n";

    f.close();
    return true;
}

// ---- public ----

void GdalVsiProcessor::registerPixelFunctions()
{
    GDALAddDerivedBandPixelFunc("amplitude", amplitudePixelFunc);
}

QString GdalVsiProcessor::process(const QString& vsiPath, const QString& outputBasePath)
{
    GDALDatasetH srcDS = GDALOpen(vsiPath.toUtf8().constData(), GA_ReadOnly);
    if (!srcDS) return QString();

    GDALRasterBandH hBand = GDALGetRasterBand(srcDS, 1);
    GDALDataType srcType = GDALGetRasterDataType(hBand);
    int w = GDALGetRasterXSize(srcDS), h = GDALGetRasterYSize(srcDS);
    QString result;

    if (GDALDataTypeIsComplex(srcType)) {
        // 从源数据集提取 GCP (地理定位)
        TiffHeaderInfo info;
        info.width = w; info.height = h;
        int nGCPs = GDALGetGCPCount(srcDS);
        if (nGCPs > 0) {
            const GDAL_GCP* gcps = GDALGetGCPs(srcDS);
            for (int i = 0; i < nGCPs; ++i) {
                TiffHeaderInfo::GcpEntry g;
                g.pixel  = gcps[i].dfGCPPixel;
                g.line   = gcps[i].dfGCPLine;
                g.lon    = gcps[i].dfGCPX;
                g.lat    = gcps[i].dfGCPY;
                g.height = gcps[i].dfGCPZ;
                info.gcps.append(g);
            }
            const char* gcpProj = GDALGetGCPProjection(srcDS);
            if (gcpProj) info.projectionWkt = QString::fromUtf8(gcpProj);
        }
        GDALClose(srcDS); srcDS = nullptr;

        QString vrtPath = outputBasePath + ".vrt";
        if (createAmplitudeVRT(vsiPath, vrtPath, w, h, info)) {
            // 强制全量计算统计值
            GDALDatasetH vrtDS = GDALOpen(vrtPath.toUtf8().constData(), GA_Update);
            if (vrtDS) {
                GDALRasterBandH vrtBand = GDALGetRasterBand(vrtDS, 1);
                double dmin, dmax, dmean, dstd;
                if (GDALGetRasterStatistics(vrtBand, FALSE, TRUE,
                        &dmin, &dmax, &dmean, &dstd) == CE_None) {
                    GDALSetRasterStatistics(vrtBand, dmin, dmax, dmean, dstd);
                }
                GDALClose(vrtDS);
            }
            result = vrtPath;
        }
    } else {
        QString tifPath = outputBasePath + ".tif";
        GDALDriverH gtDrv = GDALGetDriverByName("GTiff");
        if (gtDrv) {
            GDALDatasetH dstDS = GDALCreateCopy(gtDrv,
                tifPath.toUtf8().constData(), srcDS, FALSE,
                nullptr, nullptr, nullptr);
            if (dstDS) {
                GDALRasterBandH dstBand = GDALGetRasterBand(dstDS, 1);
                int bHasNoData = 0;
                double srcND = GDALGetRasterNoDataValue(dstBand, &bHasNoData);
                if (!bHasNoData || srcND != 0.0)
                    GDALSetRasterNoDataValue(dstBand, 0.0);
                double dmin, dmax, dmean, dstd;
                if (GDALGetRasterStatistics(dstBand, FALSE, TRUE,
                        &dmin, &dmax, &dmean, &dstd) == CE_None) {
                    GDALSetRasterStatistics(dstBand, dmin, dmax, dmean, dstd);
                }
                int levels[] = {2, 4, 8, 16, 32, 64};
                GDALBuildOverviews(dstDS, "NEAREST", 6, levels, 0, nullptr, nullptr, nullptr);
                GDALClose(dstDS);
                result = tifPath;
            }
        }
    }
    if (srcDS) GDALClose(srcDS);
    return result;
}
