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

// Step: 去平地 (合并产品上)
//   ⚠ 2026-08-16 定案: 本管线配准的距离偏移模型由轨道几何驱动 —
//   重采样本身已补偿平地差分距离 (实测残余距离相位梯度 −0.007 rad/px ≈ 0,
//   Python 独立验证)。旧版 φ_flat=−4π/λ·Bpar·sinθ 模型对一般基线几何上是
//   错误的 (缺垂直基线项), 且此时会双重移除 — 已删除旋转, 本步退化为纯拷贝。
//   (保留步骤结构以维持产品链与 qsar 记录稳定。)
bool FlatEarthRemover::execute(IfgPipelineContext& ctx)
{
    const QString ifgPath = ctx.mergeOutputBase + "_ifg.tif";

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

    const QString flatPath = outBase + "_flat.tif";
    const QString phasePath = outBase + "_flat_phase.tif";
    GDALDatasetH hOut = GDALCreate(drv, flatPath.toUtf8().constData(), w, h, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hPh  = GDALCreate(drv, phasePath.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!hOut || !hPh) { ctx.errorMessage = "FlatEarthRemover: cannot create output"; return false; }
    GDALSetGeoTransform(hOut, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowBuf(w);
    QVector<float> rowPhase(w);

    for (int row = 0; row < h; ++row) {
        if (mCancelled) { GDALClose(hOut); GDALClose(hPh); return false; }
        auto rowData = reader.readBandWindow(0, 0, row, w, 1);
        for (int col = 0; col < w; ++col) {
            const auto v = rowData.size() > col ? rowData[col] : std::complex<float>(0, 0);
            rowBuf[col] = v;
            rowPhase[col] = std::atan2(v.imag(), v.real());
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
    qDebug() << "[FlatEarth] SUCCESS (passthrough copy — flat-earth phase already"
             << "compensated by geometry-driven registration offsets)" << w << "x" << h;
    return true;
}
