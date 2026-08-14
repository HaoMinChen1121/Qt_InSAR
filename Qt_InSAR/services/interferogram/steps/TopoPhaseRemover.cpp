#include "TopoPhaseRemover.h"
#include "../PipelineContext.h"
#include "GeomTable.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/GdalDemReader.h"
#include "domain/SarComplexTypes.h"
#include "services/interferogram/dem/DemMapper.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Stage 2: 差分干涉 (在合并的 flat 产品上)
//   φ_topo = −4π/λ · B⊥ · h / (R·sinθ)
//   DEM 采样经 IDemMapper 抽象 (v2 = LinearSlantRangeMapper)
bool TopoPhaseRemover::execute(IfgPipelineContext& ctx)
{
    const QString flatSrc = ctx.flatSourcePath.isEmpty()
        ? ctx.mergeOutputBase + "_ifg.tif" : ctx.flatSourcePath;
    const QString demPath = ctx.params->demPath;

    GeomTable geom;
    if (!geom.load(ctx.geomTablePath)) {
        ctx.errorMessage = QStringLiteral("TopoPhaseRemover: cannot load geom table %1")
            .arg(ctx.geomTablePath);
        return false;
    }

    GdalSlcReader reader;
    if (!reader.open(flatSrc)) {
        ctx.errorMessage = QStringLiteral("TopoPhaseRemover: cannot open input");
        return false;
    }
    const int w = reader.width(), h = reader.height();

    GdalDemReader dem;
    if (!dem.open(demPath)) {
        ctx.errorMessage = "TopoPhaseRemover: cannot open DEM";
        reader.close(); return false;
    }

    std::unique_ptr<IDemMapper> mapper(
        new LinearSlantRangeMapper(geom, &dem, w, h));

    const QString outBase = ctx.diffOutputBase;
    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    const QString diffPath = outBase + "_diff.tif";
    const QString phasePath = outBase + "_diff_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, diffPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) { ctx.errorMessage = "TopoPhaseRemover: cannot create output"; reader.close(); dem.close(); return false; }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    const SarSensorInfo& si = ctx.masterSensorInfo;
    const double wavelength = si.wavelength > 0 ? si.wavelength : ctx.params->wavelength;
    const double Bperp = ctx.params->baselinePerp;
    const double k = -4.0 * M_PI / std::max(wavelength, 1e-9) * Bperp;

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);

    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close(); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            double R = 0, theta = 0, hDem = 0;
            double phiTopo = 0.0;
            if (geom.colGeometry(col, &R, &theta)
                && mapper->sample(row, col, &hDem)) {
                phiTopo = k * hDem / (R * std::sin(theta));
            }
            const float c = std::cos(static_cast<float>(phiTopo));
            const float s = std::sin(static_cast<float>(phiTopo));
            const auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            const auto diffVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = diffVal;
            rowPhase[col] = std::atan2(diffVal.imag(), diffVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1, reinterpret_cast<CFloat32*>(rowBuf.data()), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1, rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh); reader.close(); dem.close();

    ctx.outputBand.diffFile = QStringLiteral("diff/S1_%1_diff.tif")
        .arg(ctx.outputBand.polarization);
    ctx.outputBand.diffPhaseFile = QStringLiteral("diff/S1_%1_diff_phase.tif")
        .arg(ctx.outputBand.polarization);
    qDebug() << "[TopoPhase] SUCCESS (merged, mapper=" << mapper->name().toUtf8().constData()
             << ")" << w << "x" << h << "->" << diffPath;
    return true;
}
