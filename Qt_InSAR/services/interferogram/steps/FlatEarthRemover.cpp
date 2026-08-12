#include "FlatEarthRemover.h"
#include "../PipelineContext.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDir>
#include <QFileInfo>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool FlatEarthRemover::execute(IfgPipelineContext& ctx)
{
    QString ifgPath = ctx.ifgOutputBase + "_ifg.tif";
    QString outBase = ctx.params->outputDir + "/flat/"
                      + ctx.outputBand.subSwath + "_" + ctx.outputBand.polarization;

    GdalSlcReader reader;
    if (!reader.open(ifgPath)) {
        ctx.errorMessage = QStringLiteral("FlatEarthRemover: cannot open interferogram");
        return false;
    }
    int w = reader.width(), h = reader.height();

    QDir().mkpath(QFileInfo(outBase).absolutePath());
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    QString flatPath = outBase + "_flat.tif";
    QString phasePath = outBase + "_flat_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, flatPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) { ctx.errorMessage = "FlatEarthRemover: cannot create output"; return false; }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    const SarSensorInfo& si = ctx.masterSensorInfo;
    double wavelength = si.wavelength > 0 ? si.wavelength : ctx.params->wavelength;
    double nearRange  = si.nearRange > 0  ? si.nearRange  : ctx.params->nearRange;
    double rangeSpacing = si.rangeSpacing > 0 ? si.rangeSpacing : ctx.params->rangeSpacing;
    double Bpar = ctx.params->baselinePar;
    // 入射角沿 range 线性变化: 优先使用 XML 解析的 near/far
    double incNearDeg = si.incidenceAngleNear > 0 ? si.incidenceAngleNear
                      : si.incidenceAngleMid > 0  ? si.incidenceAngleMid - 5.0
                      : ctx.params->incidenceAngle - 5.0;
    double incFarDeg  = si.incidenceAngleFar > 0 ? si.incidenceAngleFar
                      : si.incidenceAngleMid > 0  ? si.incidenceAngleMid + 5.0
                      : ctx.params->incidenceAngle + 5.0;

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);

    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            // 距离变平板相位: φ_flat(R) = -4π/λ * B∥ * sin(θ(R))
            double incDeg = incNearDeg + (incFarDeg - incNearDeg) * col / (w - 1 + 1e-9);
            double incRad = incDeg * M_PI / 180.0;
            double phiFlat = -4.0 * M_PI / wavelength * Bpar * std::sin(incRad);
            float c = std::cos(static_cast<float>(phiFlat));
            float s = std::sin(static_cast<float>(phiFlat));
            auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0,0);
            auto flatVal = std::complex<float>(
                v.real() * c + v.imag() * s,
                v.imag() * c - v.real() * s);
            rowBuf[col] = flatVal;
            rowPhase[col] = std::atan2(flatVal.imag(), flatVal.real());
        }
        GDALRasterIO(GDALGetRasterBand(hOut,1), GF_Write, 0, row, w, 1,
            reinterpret_cast<CFloat32*>(rowBuf.data()), w, 1, GDT_CFloat32, 0, 0);
        GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, w, 1,
            rowPhase.data(), w, 1, GDT_Float32, 0, 0);
    }
    GDALClose(hOut); GDALClose(hPh);

    ctx.flatSourcePath = flatPath;
    ctx.outputBand.flatFile = QStringLiteral("flat/%1_%2_flat.tif")
        .arg(ctx.outputBand.subSwath).arg(ctx.outputBand.polarization);
    ctx.outputBand.flatPhaseFile = QStringLiteral("flat/%1_%2_flat_phase.tif")
        .arg(ctx.outputBand.subSwath).arg(ctx.outputBand.polarization);
    return true;
}
