#include "IfgGenerator.h"
#include "../PipelineContext.h"
#include "algorithms/DerampCore.h"
#include "algorithms/RangeSpectralFilter.h"
#include "dataaccess/impl/GdalSlcReader.h"
#include "dataaccess/impl/SentinelDataReader.h"
#include "dataaccess/ISarProduct.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Step 1: 复干涉图 + 相干性 + 相位 — 多视 + 逐 burst 干涉
// (TOPSAR: 逐 burst 写 3 波段临时块, 拼接/方位校正在 TopsarDeburst;
//  非 TOPSAR: 直写 deburst 输出)
bool IfgGenerator::execute(IfgPipelineContext& ctx)
{
    qDebug() << "[Ifg] stageInterferogram: master=" << ctx.masterPath.left(80);
    qDebug() << "[Ifg] stageInterferogram: slave=" << ctx.slavePath;

    // Master: 优先用 SentinelDataReader (快速 ZIP 读取), 回退 GdalSlcReader
    SentinelDataReader mSdr;
    GdalSlcReader mReader, sReader;
    bool useSdr = !ctx.masterZip.isEmpty() && !ctx.masterEntry.isEmpty();
    if (useSdr) {
        SarBandDescriptor dummyBand;
        dummyBand.burstCount = 0;  // SDR 不用于 burst 缓存, 仅用于 open
        if (!mSdr.open(ctx.masterZip, ctx.masterEntry, dummyBand)) {
            qWarning() << "[Ifg] SDR open failed, fallback to GdalSlcReader";
            useSdr = false;
        }
    }
    if (!useSdr) {
        if (!mReader.open(ctx.masterPath)) {
            ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open master");
            return false;
        }
    }
    if (!sReader.open(ctx.slavePath)) {
        ctx.errorMessage = QStringLiteral("IfgGenerator: cannot open slave");
        return false;
    }

    int realW = useSdr ? mSdr.width() : mReader.width();
    int realH = useSdr ? mSdr.height() : mReader.height();
    int rgLooks = ctx.params->rangeLooks;
    int azLooks = ctx.params->azimuthLooks;
    int outW = realW / rgLooks;
    if (outW < 1) { ctx.errorMessage = "IfgGenerator: outW < 1"; return false; }

    qDebug() << "[Ifg-CK1] master" << realW << "x" << realH
             << "slave" << sReader.width() << "x" << sReader.height();

    bool isTopsar = ctx.burstInfo && ctx.burstInfo->burstCount > 1;

    GDALDriverH driver = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    QVector<std::complex<float>> rowComplex(outW);
    QVector<float> rowPhase(outW);
    QVector<float> rowCoh(outW);
    int cohWindow = 5;

    if (isTopsar) {
        // ── TOPSAR: 逐 burst 完整多视 → 临时块 ──
        // 4 波段 Float32 (单次 GDALCreate, 不依赖 GDALAddBand — 旧版 GDAL 不支持):
        //   band1=ifg 实部, band2=ifg 虚部, band3=coh, band4=phase
        int N = ctx.burstInfo->burstCount;
        int L = ctx.burstInfo->linesPerBurst;
        int burstOutH = L / azLooks;   // 完整 burst (重叠裁剪由 TopsarDeburst 完成)
        if (burstOutH < 1) { ctx.errorMessage = "IfgGenerator: burstOutH < 1"; return false; }

        QVector<float> rowRe(outW);
        QVector<float> rowIm(outW);
        QVector<std::complex<float>> burstIfg;   // 整 burst 干涉图 (斜坡估计后统一写)
        // 多视窗相位浓度诊断: |Σ m·s*|/Σ|m·s*| — 窗内相位是否相干
        // (残余 chirp/地形相位在 8×8 窗内变化会摧毁复平均 → 相位噪声底)
        std::complex<double> winSum(0, 0);
        double winMagSum = 0.0;
        for (int b = 0; b < N; ++b) {
            if (mCancelled) return false;

            int burstRow0 = b * L;
            int readRow0 = burstRow0 - cohWindow;
            int readH = L + cohWindow * 2;
            if (readRow0 < 0) { readH += readRow0; readRow0 = 0; }
            if (readRow0 + readH > realH) readH = realH - readRow0;

            QVector<std::complex<float>> mBurst(realW * readH);
            QVector<std::complex<float>> sBurst;
            if (useSdr)
                mSdr.readWindow(0, readRow0, realW, readH, mBurst.data());
            else
                mBurst = mReader.readBandWindow(0, 0, readRow0, realW, readH);
            sBurst = sReader.readBandWindow(0, 0, readRow0, realW, readH);
            int actualH = std::min(mBurst.size() / realW, sBurst.size() / realW);

            // ── 距离向公共频带滤波 (基线频谱去相关消除, 2026-08-16) ──
            // B⊥=3310m 时主辅距离频谱平移 Δf = f0·|B⊥|/(R·tanθ) ≈ 29 MHz,
            // 与 56.5 MHz 带宽重叠仅 49% → 平移不变的相干损失 (2D 亚像素
            // 搜索实测相干面全平 0.23 的根因)。两侧各保留公共频带,
            // 外缘余弦渐变。
            {
                const double bw = ctx.masterSensorInfo.rangeSamplingRate > 0
                    ? ctx.masterSensorInfo.rangeSamplingRate * 0.878
                    : 56.5e6;   // S1 IW: 56.5 MHz 带宽 @ 64.345 MHz 采样
                const double Bp = ctx.params->baselinePerp;
                const double lambda = ctx.masterSensorInfo.wavelength > 0
                    ? ctx.masterSensorInfo.wavelength : 0.05546576;
                const double f0 = 299792458.0 / lambda;
                const double Rmid = ctx.masterNearRange > 0
                    ? ctx.masterNearRange + (realW / 2.0) * ctx.masterRangeSpacing
                    : 880000.0;
                constexpr double H = 693000.0, Re = 6378137.0;
                const double cosT = (Rmid * Rmid + 2.0 * H * Re + H * H)
                    / (2.0 * Rmid * (H + Re));
                const double sinT = std::sqrt(std::max(0.0, 1.0 - cosT * cosT));
                const double tanT = sinT / std::max(1e-9, cosT);
                const double deltaF = f0 * std::abs(Bp) / (Rmid * tanT);
                sar::RangeSpectralFilter rsf;
                if (rsf.init(realW, bw, deltaF)) {
                    for (int r = 0; r < actualH; ++r) {
                        rsf.apply(mBurst.data() + static_cast<size_t>(r) * realW);
                        rsf.apply(sBurst.data() + static_cast<size_t>(r) * realW);
                    }
                    if (b == 0)
                        qDebug() << "[Ifg] range common-band filter: Bp="
                                 << QString::number(Bp, 'f', 1) << "m deltaF="
                                 << QString::number(deltaF / 1e6, 'f', 2)
                                 << "MHz (bw=" << QString::number(bw / 1e6, 'f', 1)
                                 << "MHz)";
                } else if (b == 0) {
                    qDebug() << "[Ifg] range common-band filter skipped";
                }
            }

            // ── 主影像方位 deramp (与 DerampCore::applyDeramp_SoA 同约定) ──
            // 原始 TOPS 相位 = exp(−jπ·kt·η²) (kt=annotation fmRate, 负),
            // 旋转 exp(+jπ·kt·η²) 展平; η 以 burst 中心为原点.
            // 辅影像在配准 SincResampler 写出时已按同约定 deramp 平坦 —
            // 主影像不展平则干涉图残留 chirp → 相干摧毁 (实测教训)
            // kt 用数据实测值: annotation azimuthFmRate 与数据真实 chirp
            // 可差 ~750 Hz/s (2026-08-15 实测 −2195.78 vs −1450)
            double prfDeramp = 0.0;
            if (ctx.masterBurstInfo && ctx.masterBurstInfo->azimuthFrequency > 0)
                prfDeramp = ctx.masterBurstInfo->azimuthFrequency;
            else if (ctx.masterSensorInfo.prf > 0)
                prfDeramp = ctx.masterSensorInfo.prf;
            const int mL = (ctx.masterBurstInfo && ctx.masterBurstInfo->linesPerBurst > 0)
                ? ctx.masterBurstInfo->linesPerBurst : L;
            const int mBurstRow0 = (ctx.masterBurstInfo
                && ctx.masterBurstInfo->burstStartLines.size() > b)
                ? ctx.masterBurstInfo->burstStartLines[b] - 1 : burstRow0;
            double ktDeramp = ctx.azimuthFmRate;
            if (prfDeramp > 0 && actualH > 64) {
                double conc = 0;
                // centerRow 为窗口相对坐标 (r ∈ [0, actualH))
                const double centerWin = (mBurstRow0 + mL / 2.0) - readRow0;
                const double ktMeas = sar::measureAzimuthFmRateAos(
                    mBurst.data(), realW, actualH, prfDeramp,
                    centerWin, &conc);
                if (conc > 0.2 && conc < 1.5) {
                    ktDeramp = ktMeas;
                    if (b == 0)
                        qDebug() << "[Ifg] master measured kt=" << ktMeas
                                 << "(conc=" << conc << ") vs annotation"
                                 << ctx.azimuthFmRate;
                }
            }
            if (std::abs(ktDeramp) > 1e-9 && prfDeramp > 0) {
                // 距离相关调频率 (两段模型, 2026-08-15 两轮修正):
                //   kt_eff(R) = A/R + B (A=轨道项, B=转向项距离无关);
                //   kt(c) = ktDeramp + ctx.azimuthFmRate·(Rmid/R(c) − 1)
                //   (ktDeramp=实测列平均=A/Rmid+B, annotation=A/Rmid 近似)。
                //   简单 1/R 缩放会把转向项 B 也缩放 → 边缘残余 ±454 rad。
                //   16 列分块: 块内 kt 变化 <0.02%, 残余相位 <0.2 rad
                const bool hasGeom = ctx.masterNearRange > 0.0
                    && ctx.masterRangeSpacing > 0.0;
                const double Rmid = hasGeom
                    ? ctx.masterNearRange + (realW / 2.0) * ctx.masterRangeSpacing
                    : 0.0;
                constexpr int kColBlock = 16;
                for (int r = 0; r < actualH; ++r) {
                    const double eta = (readRow0 + r - mBurstRow0 - mL / 2.0) / prfDeramp;
                    const double base = M_PI * eta * eta;   // 乘 ktEff 得 +π·kt·η²
                    std::complex<float>* row = mBurst.data() + static_cast<size_t>(r) * realW;
                    for (int c0 = 0; c0 < realW; c0 += kColBlock) {
                        const int n = c0 + kColBlock < realW ? kColBlock : realW - c0;
                        double ktEff = ktDeramp;
                        if (hasGeom) {
                            const double Rc = ctx.masterNearRange
                                + (c0 + n * 0.5) * ctx.masterRangeSpacing;
                            const double f = Rmid / Rc;
                            ktEff = (ctx.azimuthFmRate != 0.0)
                                ? ktDeramp + ctx.azimuthFmRate * (f - 1.0)
                                : ktDeramp * f;
                        }
                        const double dp = base * ktEff;
                        const float dCos = static_cast<float>(std::cos(dp));
                        const float dSin = static_cast<float>(std::sin(dp));
                        for (int c = c0; c < c0 + n; ++c) {
                            const std::complex<float> v = row[c];
                            row[c] = { v.real() * dCos - v.imag() * dSin,
                                       v.real() * dSin + v.imag() * dCos };
                        }
                    }
                }
            }

            // ── 解析差分多普勒旋转 (多视前! 2026-08-16 关键顺序修正) ──
            // ifg 的线性方位相位 2π·Δf_DC(c)·η 在多视 8×8 窗内变化 ~3 rad
            // (Δf~30Hz, 窗 0.0164s) → 摧毁复平均 (窗浓度实测 0.249)。
            // 旋转必须在多视平均之前作用于全分辨率数据 —
            // 多视后再旋转已无法恢复被平均摧毁的相位。
            {
                constexpr double kC0 = 299792458.0;
                const bool hasMasterDc = b < ctx.masterDcPoly.size()
                    && !ctx.masterDcPoly[b].isEmpty();
                const bool hasSlaveDc = b < ctx.slaveDcPoly.size()
                    && !ctx.slaveDcPoly[b].isEmpty();
                const bool hasGeom = ctx.masterNearRange > 0.0
                    && ctx.masterRangeSpacing > 0.0;
                auto evalPoly = [](const QVector<double>& p, double dt) {
                    double v = 0.0;
                    for (int k = p.size() - 1; k >= 0; --k) v = v * dt + p[k];
                    return v;
                };
                if (hasMasterDc && hasSlaveDc && hasGeom && prfDeramp > 0) {
                    const double t0m = b < ctx.masterDcT0.size() ? ctx.masterDcT0[b] : 0.0;
                    const double t0s = b < ctx.slaveDcT0.size() ? ctx.slaveDcT0[b] : 0.0;
                    constexpr int kColBlock = 16;
                    for (int r = 0; r < actualH; ++r) {
                        const double eta = (readRow0 + r - mBurstRow0 - mL / 2.0) / prfDeramp;
                        std::complex<float>* row = mBurst.data() + static_cast<size_t>(r) * realW;
                        for (int c0 = 0; c0 < realW; c0 += kColBlock) {
                            const int n = c0 + kColBlock < realW ? kColBlock : realW - c0;
                            const double R = ctx.masterNearRange
                                + (c0 + n * 0.5) * ctx.masterRangeSpacing;
                            const double tau = 2.0 * R / kC0;
                            const double df = evalPoly(ctx.masterDcPoly[b], tau - t0m)
                                            - evalPoly(ctx.slaveDcPoly[b], tau - t0s);
                            const double ph = -2.0 * M_PI * df * eta;
                            const float dCos = static_cast<float>(std::cos(ph));
                            const float dSin = static_cast<float>(std::sin(ph));
                            for (int c = c0; c < c0 + n; ++c) {
                                const std::complex<float> v = row[c];
                                row[c] = { v.real() * dCos - v.imag() * dSin,
                                           v.real() * dSin + v.imag() * dCos };
                            }
                        }
                    }
                    if (b == 0)
                        qDebug() << "[Ifg] analytic diff-Doppler rotation applied pre-multilook";
                } else if (b == 0) {
                    qWarning() << "[Ifg] analytic diff-Doppler skipped (DC data missing,"
                               << "master=" << hasMasterDc << "slave=" << hasSlaveDc << ")";
                }
            }

            QString blkPath = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 1);
            GDALDatasetH hBlk = GDALCreate(driver, blkPath.toUtf8().constData(),
                                           outW, burstOutH, 4, GDT_Float32, nullptr);
            if (!hBlk) {
                ctx.errorMessage = QStringLiteral("IfgGenerator: cannot create burst block %1").arg(blkPath);
                return false;
            }
            GDALSetGeoTransform(hBlk, gt);

            qDebug() << "[Ifg] burst" << (b+1) << "/" << N
                     << "blockRows=" << burstOutH;

            burstIfg.resize(static_cast<size_t>(outW) * burstOutH);

            for (int j = 0; j < burstOutH; ++j) {
                int srcRow = burstRow0 + j * azLooks;
                int rowOff = srcRow - readRow0;

                for (int col = 0; col < outW; ++col) {
                    int srcCol = col * rgLooks;

                    std::complex<double> mAvg(0, 0), sAvg(0, 0);
                    int nPix = 0;
                    for (int ar = 0; ar < azLooks; ++ar) {
                        for (int ac = 0; ac < rgLooks; ++ac) {
                            int idx = (rowOff + ar) * realW + (srcCol + ac);
                            if (idx >= 0 && idx < actualH * realW) {
                                mAvg += std::complex<double>(mBurst[idx].real(), mBurst[idx].imag());
                                sAvg += std::complex<double>(sBurst[idx].real(), sBurst[idx].imag());
                                ++nPix;
                            }
                        }
                    }
                    if (nPix > 0) { mAvg /= nPix; sAvg /= nPix; }

                    std::complex<double> ifg = mAvg * std::conj(sAvg);
                    burstIfg[static_cast<size_t>(j) * outW + col] =
                        std::complex<float>(static_cast<float>(ifg.real()),
                                            static_cast<float>(ifg.imag()));

                    // 多视窗相位浓度 (诊断, 在 8×8 窗内累计)
                    {
                        std::complex<double> wSum(0, 0);
                        double wMag = 0;
                        for (int ar = 0; ar < azLooks; ++ar) {
                            for (int ac = 0; ac < rgLooks; ++ac) {
                                int idx = (rowOff + ar) * realW + (srcCol + ac);
                                if (idx >= 0 && idx < actualH * realW) {
                                    auto mv = mBurst[idx]; auto sv = sBurst[idx];
                                    wSum += std::complex<double>(mv.real(), mv.imag())
                                        * std::complex<double>(sv.real(), -sv.imag());
                                    wMag += std::sqrt((mv.real()*mv.real() + mv.imag()*mv.imag())
                                        * (sv.real()*sv.real() + sv.imag()*sv.imag()));
                                }
                            }
                        }
                        winSum += std::complex<double>(std::abs(wSum), 0.0);
                        winMagSum += wMag;
                    }

                    std::complex<double> crossSum(0, 0);
                    double magM = 0, magS = 0;
                    for (int wr = -cohWindow/2; wr <= cohWindow/2; ++wr) {
                        for (int wc = -cohWindow/2; wc <= cohWindow/2; ++wc) {
                            int sc = srcCol + cohWindow/2 + wc;
                            int sr = rowOff + cohWindow/2 + wr;
                            if (sc >= 0 && sc < realW && sr >= 0 && sr < actualH) {
                                int idx = sr * realW + sc;
                                auto mv = mBurst[idx]; auto sv = sBurst[idx];
                                crossSum += std::complex<double>(mv.real(), mv.imag())
                                    * std::complex<double>(sv.real(), -sv.imag());
                                magM += mv.real()*mv.real() + mv.imag()*mv.imag();
                                magS += sv.real()*sv.real() + sv.imag()*sv.imag();
                            }
                        }
                    }
                    double denom = std::sqrt(std::max(1e-15, magM * magS));
                    // 零填充区守卫: 单非零像素窗会得 coh=1 退化值 (配准输出
                    // 的 burst 边界零行进入后渲染为白色伪影), 幅度过小 → coh=0
                    rowCoh[col] = (magM < 1e-3 || magS < 1e-3)
                        ? 0.0f
                        : static_cast<float>(std::abs(crossSum) / denom);
                }
                GDALRasterIO(GDALGetRasterBand(hBlk, 3), GF_Write, 0, j, outW, 1,
                    rowCoh.data(), outW, 1, GDT_Float32, 0, 0);
            }

            // 写 band1/2/4 (解析旋转已在多视前应用)
            for (int j = 0; j < burstOutH; ++j) {
                const auto* row = burstIfg.data() + static_cast<size_t>(j) * outW;
                for (int c = 0; c < outW; ++c) {
                    rowRe[c] = row[c].real();
                    rowIm[c] = row[c].imag();
                    rowPhase[c] = std::atan2(row[c].imag(), row[c].real());
                }
                GDALRasterIO(GDALGetRasterBand(hBlk, 1), GF_Write, 0, j, outW, 1,
                    rowRe.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hBlk, 2), GF_Write, 0, j, outW, 1,
                    rowIm.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hBlk, 4), GF_Write, 0, j, outW, 1,
                    rowPhase.data(), outW, 1, GDT_Float32, 0, 0);
            }
            GDALClose(hBlk);
        }

        // 多视窗相位浓度汇总 (诊断: 1.0=窗内相位相干, 低值=chirp/地形残余
        // 在 8×8 窗内变化摧毁复平均 → 相位噪声底)
        qDebug() << "[Ifg] multilook window phase concentration ="
                 << QString::number(winMagSum > 0 ? std::abs(winSum) / winMagSum : 0.0, 'f', 3)
                 << "(1.0=coherent, low=window phase variation)";

        ctx.outWidth = outW;
        // ctx.outHeight 由 TopsarDeburst 计算
    } else {
        // ── 非 TOPSAR: 直写 deburst 输出 ──
        int outH = realH / azLooks;
        if (outH < 1) { ctx.errorMessage = "IfgGenerator: outH < 1"; return false; }

        QString base = ctx.deburstOutputBase;
        QDir().mkpath(QFileInfo(base).absolutePath());
        QString ifgPath  = base + "_ifg.tif";
        QString cohPath  = base + "_coh.tif";
        QString phasePath = base + "_phase.tif";

        GDALDatasetH hIfg = GDALCreate(driver, ifgPath.toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
        GDALDatasetH hCoh = GDALCreate(driver, cohPath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
        GDALDatasetH hPh  = GDALCreate(driver, phasePath.toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
        if (!hIfg || !hCoh || !hPh) { ctx.errorMessage = "IfgGenerator: cannot create output"; return false; }
        GDALSetGeoTransform(hIfg, gt); GDALSetGeoTransform(hCoh, gt); GDALSetGeoTransform(hPh, gt);

        for (int row = 0; row < outH; ++row) {
            if (mCancelled) { GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh); return false; }
            int srcRow = row * azLooks;
            int readH = azLooks + cohWindow * 2;
            int row0 = srcRow - cohWindow;

            QVector<std::complex<float>> mData(realW * readH);
            if (useSdr)
                mSdr.readWindow(0, row0, realW, readH, mData.data());
            else
                mData = mReader.readBandWindow(0, 0, row0, realW, readH);
            auto sData = sReader.readBandWindow(0, 0, row0, realW, readH);
            int actualH = std::min(mData.size() / realW, sData.size() / realW);
            if (actualH == 0) {
                rowComplex.fill(std::complex<float>(0,0));
                rowPhase.fill(0); rowCoh.fill(0);
                GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, reinterpret_cast<CFloat32*>(rowComplex.data()), outW, 1, GDT_CFloat32, 0, 0);
                GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
                GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, row, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
                continue;
            }

            int rowOff = cohWindow + (row0 < 0 ? row0 : 0);

            for (int col = 0; col < outW; ++col) {
                int srcCol = col * rgLooks;

                std::complex<double> mAvg(0, 0), sAvg(0, 0);
                for (int ar = 0; ar < azLooks; ++ar) {
                    for (int ac = 0; ac < rgLooks; ++ac) {
                        int idx = (rowOff + ar) * realW + (srcCol + ac);
                        if (idx >= 0 && idx < actualH * realW) {
                            mAvg += std::complex<double>(mData[idx].real(), mData[idx].imag());
                            sAvg += std::complex<double>(sData[idx].real(), sData[idx].imag());
                        }
                    }
                }
                int nPix = azLooks * rgLooks;
                mAvg /= nPix;
                sAvg /= nPix;

                std::complex<double> ifg = mAvg * std::conj(sAvg);
                rowComplex[col] = std::complex<float>(ifg.real(), ifg.imag());
                rowPhase[col] = std::atan2(ifg.imag(), ifg.real());

                std::complex<double> crossSum(0, 0);
                double magM = 0, magS = 0;
                for (int wr = -cohWindow/2; wr <= cohWindow/2; ++wr) {
                    for (int wc = -cohWindow/2; wc <= cohWindow/2; ++wc) {
                        int sc = srcCol + cohWindow/2 + wc;
                        int sr = rowOff + cohWindow/2 + wr;
                        if (sc >= 0 && sc < realW && sr >= 0 && sr < actualH) {
                            int idx = sr * realW + sc;
                            auto mv = mData[idx]; auto sv = sData[idx];
                            crossSum += std::complex<double>(mv.real(), mv.imag())
                                * std::complex<double>(sv.real(), -sv.imag());
                            magM += mv.real()*mv.real() + mv.imag()*mv.imag();
                            magS += sv.real()*sv.real() + sv.imag()*sv.imag();
                        }
                    }
                }
                double denom = std::sqrt(std::max(1e-15, magM * magS));
                rowCoh[col] = static_cast<float>(std::abs(crossSum) / denom);
            }

            GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, row, outW, 1, reinterpret_cast<CFloat32*>(rowComplex.data()), outW, 1, GDT_CFloat32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, row, outW, 1, rowPhase.data(),    outW, 1, GDT_Float32,  0, 0);
            GDALRasterIO(GDALGetRasterBand(hCoh,1),  GF_Write, 0, row, outW, 1, rowCoh.data(),      outW, 1, GDT_Float32,  0, 0);
        }

        GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);
        ctx.outWidth  = outW;
        ctx.outHeight = outH;
    }

    qDebug() << "[Ifg] stageInterferogram SUCCESS";
    return true;
}
