#include "TopoPhaseRemover.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalDemReader.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool TopoPhaseRemover::execute(IfgPipelineContext& ctx)
{
    QString flatSrc = ctx.flatSourcePath.isEmpty()
        ? ctx.ifgOutputBase + "_ifg.tif" : ctx.flatSourcePath;
    QString demPath = ctx.params->demPath;
    QString outBase = ctx.params->outputDir + "/diff/"
                      + ctx.outputBand.subSwath + "_" + ctx.outputBand.polarization;

    GdalSlcReader reader;
    if (!reader.open(flatSrc)) {
        ctx.errorMessage = QStringLiteral("TopoPhaseRemover: cannot open input");
        return false;
    }
    int w = reader.width(), h = reader.height();

    GdalDemReader dem;
    if (!dem.open(demPath)) {
        ctx.errorMessage = "TopoPhaseRemover: cannot open DEM";
        reader.close(); return false;
    }
    int demW = dem.width(), demH = dem.height();

    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    QString diffPath = outBase + "_diff.tif";
    QString phasePath = outBase + "_diff_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, diffPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) { ctx.errorMessage = "TopoPhaseRemover: cannot create output"; reader.close(); dem.close(); return false; }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    double wavelength = ctx.params->wavelength;
    double nearRange = ctx.params->nearRange;
    double rangeSpacing = ctx.params->rangeSpacing;
    double incRad = ctx.params->incidenceAngle * M_PI / 180.0;
    double Bperp = ctx.params->baselinePerp;

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);
    QVector<float> demRowData(demW);
    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close(); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        int demRow = row * demH / h;
        demRow = qBound(0, demRow, demH - 1);
        auto demLine = dem.readElevationWindow(0, demRow, demW, 1);
        if (demLine.size() >= demW) demRowData = demLine;

        for (int col = 0; col < w; ++col) {
            double R = nearRange + col * rangeSpacing;
            int demCol = col * demW / w;
            demCol = qBound(0, demCol, demW - 1);
            double hDem = static_cast<double>(demRowData[demCol]);
            if (hDem < -1000.0) hDem = 0.0;
            double phiTopo = -4.0 * M_PI / wavelength * Bperp * hDem / (R * std::sin(incRad));

            float c = std::cos(static_cast<float>(phiTopo));
            float s = std::sin(static_cast<float>(phiTopo));
            auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            auto diffVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = diffVal;
            rowPhase[col] = std::atan2(diffVal.imag(), diffVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1, rowBuf.data(), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1, rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close();

    ctx.outputBand.diffFile = QStringLiteral("diff/%1_%2_diff.tif")
        .arg(ctx.outputBand.subSwath).arg(ctx.outputBand.polarization);
    ctx.outputBand.diffPhaseFile = QStringLiteral("diff/%1_%2_diff_phase.tif")
        .arg(ctx.outputBand.subSwath).arg(ctx.outputBand.polarization);
    return true;
}
