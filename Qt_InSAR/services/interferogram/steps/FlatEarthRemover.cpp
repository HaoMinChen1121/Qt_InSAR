#include "FlatEarthRemover.h"
#include "../PipelineContext.h"
#include "GeomTable.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Stage 2: 在合并产品上去平地 (逐列几何查表)
//   φ_flat(R) = −4π/λ · B∥ · sinθ(R),  R/θ 由 merge 输出的几何表逐列提供
bool FlatEarthRemover::execute(IfgPipelineContext& ctx)
{
    QString ifgPath = ctx.mergeOutputBase + "_ifg.tif";

    GeomTable geom;
    if (!geom.load(ctx.geomTablePath)) {
        ctx.errorMessage = QStringLiteral("FlatEarthRemover: cannot load geom table %1")
            .arg(ctx.geomTablePath);
        return false;
    }

    GdalSlcReader reader;
    if (!reader.open(ifgPath)) {
        ctx.errorMessage = QStringLiteral("FlatEarthRemover: cannot open merged interferogram");
        return false;
    }
    const int w = reader.width(), h = reader.height();

    const QString outBase = ctx.flatOutputBase;
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
    const double wavelength = si.wavelength > 0 ? si.wavelength : ctx.params->wavelength;
    const double Bpar = ctx.params->baselinePar;
    const double k = -4.0 * M_PI / std::max(wavelength, 1e-9) * Bpar;

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);

    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            double R = 0, theta = 0;
            double phiFlat = 0.0;
            if (geom.colGeometry(col, &R, &theta))
                phiFlat = k * std::sin(theta);
            const float c = std::cos(static_cast<float>(phiFlat));
            const float s = std::sin(static_cast<float>(phiFlat));
            const auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            const auto flatVal = std::complex<float>(
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
    ctx.outputBand.flatFile = QStringLiteral("flat/S1_%1_flat.tif")
        .arg(ctx.outputBand.polarization);
    ctx.outputBand.flatPhaseFile = QStringLiteral("flat/S1_%1_flat_phase.tif")
        .arg(ctx.outputBand.polarization);
    qDebug() << "[FlatEarth] SUCCESS (merged)" << w << "x" << h << "->" << flatPath;
    return true;
}
