#include "IfgGenerator.h"
#include "../PipelineContext.h"
#include "algorithms/DerampCore.h"
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
                for (int r = 0; r < actualH; ++r) {
                    const double eta = (readRow0 + r - mBurstRow0 - mL / 2.0) / prfDeramp;
                    const double dp = M_PI * ktDeramp * eta * eta;   // = +π·kt·η²
                    const float dCos = static_cast<float>(std::cos(dp));
                    const float dSin = static_cast<float>(std::sin(dp));
                    std::complex<float>* row = mBurst.data() + static_cast<size_t>(r) * realW;
                    for (int c = 0; c < realW; ++c) {
                        const std::complex<float> v = row[c];
                        row[c] = { v.real() * dCos - v.imag() * dSin,
                                   v.real() * dSin + v.imag() * dCos };
                    }
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

            // ── 差分多普勒斜坡消除 (数据驱动) ──
            // ifg 相位含 2π(f_DC_m − f_DC_s)·η 残余: 每景零多普勒导引的几何
            // 差分 (实测 ~92Hz = 1.19 rad/全分辨率行) — deramp 只去二次 chirp,
            // 线性多普勒项残留 → 多视盒内相位旋转 → 幅度周期调制 + 相干摧毁。
            // 逐行圆平均相位推进 → unwrap → 线性拟合 → 旋转 exp(−jβ·row)
            {
                QVector<double> adv(burstOutH - 1);
                for (int j = 0; j < burstOutH - 1; ++j) {
                    std::complex<double> s(0, 0);
                    const auto* r0p = burstIfg.data() + static_cast<size_t>(j) * outW;
                    const auto* r1p = r0p + outW;
                    for (int c = 0; c < outW; ++c) {
                        s += std::complex<double>(
                            static_cast<double>(r1p[c].real()) * r0p[c].real()
                                + static_cast<double>(r1p[c].imag()) * r0p[c].imag(),
                            static_cast<double>(r1p[c].imag()) * r0p[c].real()
                                - static_cast<double>(r1p[c].real()) * r0p[c].imag());
                    }
                    adv[j] = std::atan2(s.imag(), s.real());
                }
                // unwrap
                for (int j = 1; j < adv.size(); ++j) {
                    while (adv[j] - adv[j - 1] > M_PI) adv[j] -= 2.0 * M_PI;
                    while (adv[j] - adv[j - 1] < -M_PI) adv[j] += 2.0 * M_PI;
                }
                // 推进序列线性拟合 adv[j] ≈ a + b·j
                // 相位校正 φ_j = Σ adv = a·j + b·j(j−1)/2 —
                // 含二次项: annotation azimuthFmRate 是沿轨道变化的 11 组多项式,
                // 单一常数 kt 的 deramp 残留 ~30-80 Hz/s 二次 chirp (实测推进率
                // 线性漂移 +2.5→−6.8 rad/2行), 只去线性斜坡不够
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                const int nA = adv.size();
                for (int j = 0; j < nA; ++j) {
                    sx += j; sy += adv[j]; sxx += static_cast<double>(j) * j;
                    sxy += static_cast<double>(j) * adv[j];
                }
                const double a = (sy * sxx - sx * sxy) / (nA * sxx - sx * sx);
                const double bb = (nA * sxy - sx * sy) / (nA * sxx - sx * sx);
                // 旋转每行 exp(−jφ_j) (burst 内 0-based 行)
                for (int j = 0; j < burstOutH; ++j) {
                    const double ph = -(a * j + bb * j * (j - 1) * 0.5);
                    const float dCos = static_cast<float>(std::cos(ph));
                    const float dSin = static_cast<float>(std::sin(ph));
                    auto* row = burstIfg.data() + static_cast<size_t>(j) * outW;
                    for (int c = 0; c < outW; ++c) {
                        const std::complex<float> v = row[c];
                        row[c] = { v.real() * dCos - v.imag() * dSin,
                                   v.real() * dSin + v.imag() * dCos };
                    }
                }
                qDebug() << "[Ifg] burst" << (b+1)
                         << "phase ramp a=" << QString::number(a, 'f', 4)
                         << "rad/row b=" << QString::number(bb, 'f', 4)
                         << "rad/row^2 (a_Hz="
                         << QString::number(a * prfDeramp / (2.0 * M_PI), 'f', 2)
                         << " Hz, b_Hz/s="
                         << QString::number(bb * prfDeramp * prfDeramp / (2.0 * M_PI * azLooks), 'f', 2)
                         << ")";
            }

            // 写 band1/2/4 (旋转后)
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
