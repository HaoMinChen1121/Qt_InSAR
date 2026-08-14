#include "TopsarDeburst.h"
#include "../PipelineContext.h"
#include "domain/SarComplexTypes.h"

#include <gdal_priv.h>

#include <QtMath>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 相邻 burst 重叠行数 (master 方位时间差优先; 无时间按 ~4% 估算)
static void computeBurstDiscard(int N, int linesPerBurst, double prf,
    const QVector<QDateTime>& burstTimes,
    QVector<int>& discardTop, QVector<int>& discardBottom)
{
    discardTop.fill(0, N);
    discardBottom.fill(0, N);
    if (N < 2) return;

    if (burstTimes.size() >= N) {
        for (int b = 0; b < N - 1; ++b) {
            double dt = burstTimes[b].msecsTo(burstTimes[b+1]) / 1000.0;
            double burstDur = linesPerBurst / prf;
            double overlapTime = burstDur - dt;
            if (overlapTime < 0) overlapTime = 0;
            int overlapLines = (int)(overlapTime * prf + 0.5);
            int half = overlapLines / 2;
            discardBottom[b] = half;
            discardTop[b + 1] = half;
        }
    } else {
        int estOverlap = static_cast<int>(linesPerBurst * 0.04 + 0.5);
        int half = estOverlap / 2;
        for (int b = 0; b < N - 1; ++b) {
            discardBottom[b] = half;
            discardTop[b + 1] = half;
        }
    }
}

namespace {

// 校正多项式 (块行单位): P(j) = a + b·j + c·j²
struct Poly { double a = 0, b = 0, c = 0; };

// 加权最小二乘二次拟合 y ≈ a + b·x + c·x²
bool fitQuad(const QVector<double>& x, const QVector<double>& y,
             const QVector<double>& w, Poly* out, double* residStd)
{
    double m[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
    double r[3] = {0, 0, 0};
    const int n = x.size();
    for (int i = 0; i < n; ++i) {
        double wi = w[i], xi = x[i], yi = y[i];
        double x2 = xi * xi;
        m[0][0] += wi;         m[0][1] += wi*xi;       m[0][2] += wi*x2;
        m[1][0] += wi*xi;      m[1][1] += wi*x2;       m[1][2] += wi*xi*x2;
        m[2][0] += wi*x2;      m[2][1] += wi*xi*x2;    m[2][2] += wi*x2*x2;
        r[0] += wi*yi;         r[1] += wi*xi*yi;       r[2] += wi*x2*yi;
    }
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int row = col + 1; row < 3; ++row)
            if (std::abs(m[row][col]) > std::abs(m[piv][col])) piv = row;
        if (std::abs(m[piv][col]) < 1e-12) return false;
        if (piv != col) {
            std::swap(m[piv], m[col]);
            std::swap(r[piv], r[col]);
        }
        for (int row = col + 1; row < 3; ++row) {
            double f = m[row][col] / m[col][col];
            for (int k = col; k < 3; ++k) m[row][k] -= f * m[col][k];
            r[row] -= f * r[col];
        }
    }
    double sol[3] = {0, 0, 0};
    for (int row = 2; row >= 0; --row) {
        double v = r[row];
        for (int k = row + 1; k < 3; ++k) v -= m[row][k] * sol[k];
        sol[row] = v / m[row][row];
    }
    out->a = sol[0]; out->b = sol[1]; out->c = sol[2];
    if (residStd) {
        double res = 0, sw = 0;
        for (int i = 0; i < n; ++i) {
            double e = y[i] - (sol[0] + sol[1]*x[i] + sol[2]*x[i]*x[i]);
            res += w[i] * e * e;
            sw += w[i];
        }
        *residStd = sw > 0 ? std::sqrt(res / sw) : 0;
    }
    return true;
}

} // namespace

bool TopsarDeburst::execute(IfgPipelineContext& ctx)
{
    const QsarBand* slave = ctx.burstInfo;
    if (!slave || slave->burstCount < 2) return true;   // 直通 (服务已条件添加)

    const int N = slave->burstCount;
    const int L = std::max(1, slave->linesPerBurst);
    const int azLooks = std::max(1, ctx.params->azimuthLooks);
    const double prf = slave->azimuthFrequency > 0
        ? slave->azimuthFrequency
        : ctx.masterSensorInfo.prf > 0 ? ctx.masterSensorInfo.prf : 486.0;

    // 重叠裁剪 (master 逐 burst 方位时间优先, 无则用 slave 的)
    const QsarBand* mb = ctx.masterBurstInfo ? ctx.masterBurstInfo : slave;
    QVector<int> discardTop(N), discardBottom(N);
    computeBurstDiscard(N, L, prf, mb->burstAzimuthTimes, discardTop, discardBottom);

    // ── 校正模式解析 ──
    // off        : 不校正 (A 组)
    // annotation : 二次公式 sign·π·kt·η² (B/C 组, 符号由 INSAR_DEBURST_SIGN 控制)
    // empirical  : 重叠区实测 Δφ(η) → 链式校正 (默认, 不依赖 annotation 符号)
    enum class Mode { Off, Annotation, Empirical };
    Mode mode = Mode::Off;
    if (ctx.params->enableAzimuthRampCorrection) {
        mode = Mode::Empirical;
        QByteArray m = qgetenv("INSAR_DEBURST_MODE");
        if (m == "annotation") mode = Mode::Annotation;
        else if (m == "off") mode = Mode::Off;
    }
    int sign = ctx.azimuthRampCorrectionSign;
    if (qEnvironmentVariableIsSet("INSAR_DEBURST_SIGN"))
        sign = qEnvironmentVariableIntValue("INSAR_DEBURST_SIGN");
    const double kt = ctx.azimuthFmRate;
    qDebug() << "[Deburst] N=" << N << "L=" << L << "prf=" << prf
             << "kt=" << kt << "mode="
             << (mode == Mode::Empirical ? "empirical"
                 : mode == Mode::Annotation ? "annotation" : "off")
             << "sign=" << sign;

    // ── 输出尺寸 ──
    int outW = 0;
    {
        QString blk0 = ctx.burstBlockBase + QStringLiteral("_b1.tif");
        GDALDatasetH h0 = GDALOpen(blk0.toUtf8().constData(), GA_ReadOnly);
        if (!h0) { ctx.errorMessage = QStringLiteral("TopsarDeburst: cannot open burst block %1").arg(blk0); return false; }
        outW = GDALGetRasterXSize(h0);
        GDALClose(h0);
    }
    if (outW < 1) { ctx.errorMessage = "TopsarDeburst: outW < 1"; return false; }

    QVector<int> burstOutOffsets(N);
    int outH = 0;
    for (int b = 0; b < N; ++b) {
        burstOutOffsets[b] = outH;
        int validLines = L - discardTop[b] - discardBottom[b];
        outH += validLines / azLooks;
    }
    if (outH < 1) { ctx.errorMessage = "TopsarDeburst: outH < 1"; return false; }

    // ── per-burst 校正多项式 (块行单位) ──
    QVector<Poly> corr(N);
    const int Lb = L / azLooks;                    // 每 burst 块行数
    const double alpha = static_cast<double>(azLooks) / prf;              // 块行 → 秒
    const double beta = ((azLooks - 1) / 2.0 - L / 2.0) / prf;            // η = α·j + β (burst 中心参考)

    if (mode == Mode::Annotation) {
        // B/C 组: 二次公式 (符号 A/B/C 验证用; 预期 sign=-1, 见设计文档 §5.2)
        if (sign != 0 && std::abs(kt) > 1e-9) {
            for (int b = 0; b < N; ++b) {
                corr[b].c = sign * M_PI * kt * alpha * alpha;
                corr[b].b = sign * M_PI * kt * 2.0 * alpha * beta;
                corr[b].a = sign * M_PI * kt * beta * beta;
            }
        } else {
            qWarning() << "[Deburst] annotation 模式但 sign=0 或 k_t=0, 校正被跳过";
        }
    } else if (mode == Mode::Empirical) {
        // ── 重叠区实测: 同一方位位置的行对比, 场景相位抵消, 剩余=burst 间相位差函数 ──
        // 同时输出实测系数诊断 (回答 annotation 符号问题:
        //   实测二次系数 C 应与 annotation 预测 sign·π·kt·α² 比较)
        QVector<float> phA(outW), phB(outW), cohA(outW), cohB(outW);
        QVector<double> xs, ys, ws;
        QVector<double> seamA, seamB, seamC, seamRes;
        for (int b = 0; b < N - 1; ++b) {
            const int overlap = discardBottom[b] + discardTop[b + 1];
            const int nO = overlap / azLooks;
            const int jB = (L - overlap) / azLooks;   // burst b 重叠起始块行
            if (nO < 3 || jB < 0 || jB + nO > Lb) {
                qWarning() << "[Deburst] seam" << (b+1) << "overlap rows insufficient (nO=" << nO << ")";
                continue;
            }
            QString pathA = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 1);
            QString pathB = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 2);
            GDALDatasetH hA = GDALOpen(pathA.toUtf8().constData(), GA_ReadOnly);
            GDALDatasetH hB = GDALOpen(pathB.toUtf8().constData(), GA_ReadOnly);
            if (!hA || !hB) {
                if (hA) GDALClose(hA);
                if (hB) GDALClose(hB);
                continue;
            }
            GDALRasterBandH aPh = GDALGetRasterBand(hA, 4);
            GDALRasterBandH aCoh = GDALGetRasterBand(hA, 3);
            GDALRasterBandH bPh = GDALGetRasterBand(hB, 4);
            GDALRasterBandH bCoh = GDALGetRasterBand(hB, 3);

            xs.clear(); ys.clear(); ws.clear();
            for (int k = 0; k < nO; ++k) {
                int rA = jB + k, rB = k;
                GDALRasterIO(aPh, GF_Read, 0, rA, outW, 1, phA.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(aCoh, GF_Read, 0, rA, outW, 1, cohA.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(bPh, GF_Read, 0, rB, outW, 1, phB.data(), outW, 1, GDT_Float32, 0, 0);
                GDALRasterIO(bCoh, GF_Read, 0, rB, outW, 1, cohB.data(), outW, 1, GDT_Float32, 0, 0);
                double re = 0, im = 0, wsum = 0;
                for (int col = 0; col < outW; ++col) {
                    double w = std::min(cohA[col], cohB[col]);
                    if (w < 0.05) continue;
                    double d = phB[col] - phA[col];
                    re += w * std::cos(d);
                    im += w * std::sin(d);
                    wsum += w;
                }
                if (wsum <= 0) continue;
                xs.append(k);
                ys.append(std::atan2(im, re));
                ws.append(wsum);
            }
            GDALClose(hA);
            GDALClose(hB);

            if (xs.size() < 3) {
                qWarning() << "[Deburst] seam" << (b+1) << "有效重叠样本不足";
                continue;
            }
            for (int i = 1; i < ys.size(); ++i) {
                while (ys[i] - ys[i-1] > M_PI)  ys[i] -= 2 * M_PI;
                while (ys[i] - ys[i-1] < -M_PI) ys[i] += 2 * M_PI;
            }
            Poly fit;
            double resid = 0;
            if (!fitQuad(xs, ys, ws, &fit, &resid)) continue;

            seamA.append(fit.a); seamB.append(fit.b); seamC.append(fit.c);
            seamRes.append(resid);

            const double d = jB;
            const Poly& Pb = corr[b];
            if (resid > 25.0 * M_PI / 180.0) {
                // 质量门控: 拟合残差过大 = 噪声主导, 应用会把噪声注入校正
                // → 跳过实测项, 仅延续上一 burst 的多项式 (保持校正自身连续)
                qWarning().nospace() << "[Deburst] seam " << (b+1) << "->" << (b+2)
                    << " fit residStd=" << (resid * 180.0 / M_PI)
                    << "deg > 25deg, 实测校正跳过 (噪声主导)";
                corr[b+1].a = Pb.a + Pb.b * d + Pb.c * d * d;
                corr[b+1].b = Pb.b + 2.0 * Pb.c * d;
                corr[b+1].c = Pb.c;
                continue;
            }

            // 链式: P_{b+1}(k) = P_b(jB + k) + Δφ_measured(k)
            corr[b+1].a = Pb.a + Pb.b * d + Pb.c * d * d + fit.a;
            corr[b+1].b = Pb.b + 2.0 * Pb.c * d + fit.b;
            corr[b+1].c = Pb.c + fit.c;
        }

        // 实测系数诊断 (A/B/C 判定的直接依据)
        for (int s = 0; s < seamA.size(); ++s) {
            qDebug().nospace() << "[Deburst] overlapFit seam " << (s+1) << "->" << (s+2)
                << " A=" << (seamA[s] * 180.0 / M_PI) << "deg"
                << " B=" << (seamB[s] * 180.0 / M_PI) << "deg/row"
                << " C=" << (seamC[s] * 180.0 / M_PI) << "deg/row^2"
                << " residStd=" << (seamRes[s] * 180.0 / M_PI) << "deg";
        }
        if (!seamC.isEmpty()) {
            double bMean = 0, cMean = 0, rMean = 0;
            for (int s = 0; s < seamC.size(); ++s) {
                bMean += seamB[s]; cMean += seamC[s]; rMean += seamRes[s];
            }
            bMean /= seamC.size(); cMean /= seamC.size(); rMean /= seamC.size();
            double annC = sign * M_PI * kt * alpha * alpha;   // annotation 预测二次系数
            qDebug().nospace() << "[Deburst] EMPIRICAL seams=" << seamC.size()
                << " meanB=" << (bMean * 180.0 / M_PI) << "deg/row"
                << " meanC=" << (cMean * 180.0 / M_PI) << "deg/row^2"
                << " meanResid=" << (rMean * 180.0 / M_PI) << "deg"
                << " | annotationPredC(sign=" << sign << ")=" << (annC * 180.0 / M_PI) << "deg/row^2";
        }
    }

    // ── 拼接 (应用校正) ──
    QString base = ctx.deburstOutputBase;
    QDir().mkpath(QFileInfo(base).absolutePath());
    GDALDriverH driver = GDALGetDriverByName("GTiff");
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    GDALDatasetH hIfg = GDALCreate(driver, (base + "_ifg.tif").toUtf8().constData(), outW, outH, 1, GDT_CFloat32, nullptr);
    GDALDatasetH hCoh = GDALCreate(driver, (base + "_coh.tif").toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    GDALDatasetH hPh  = GDALCreate(driver, (base + "_phase.tif").toUtf8().constData(), outW, outH, 1, GDT_Float32, nullptr);
    if (!hIfg || !hCoh || !hPh) { ctx.errorMessage = "TopsarDeburst: cannot create output"; return false; }
    GDALSetGeoTransform(hIfg, gt); GDALSetGeoTransform(hCoh, gt); GDALSetGeoTransform(hPh, gt);

    QVector<std::complex<float>> rowComplex(outW);
    QVector<float> rowRe(outW);
    QVector<float> rowIm(outW);
    QVector<float> rowPhase(outW);
    QVector<float> rowCoh(outW);

    // 诊断: 上一 burst 尾行 (校正后边界连续性检查)
    QVector<float> prevPhase;
    QVector<float> prevCoh;
    bool havePrev = false;
    QVector<double> seamPhaseMean, seamPhaseStd, seamCohMean;

    for (int b = 0; b < N; ++b) {
        if (mCancelled) { GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh); return false; }

        QString blkPath = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 1);
        GDALDatasetH hBlk = GDALOpen(blkPath.toUtf8().constData(), GA_ReadOnly);
        if (!hBlk) {
            ctx.errorMessage = QStringLiteral("TopsarDeburst: cannot open burst block %1").arg(blkPath);
            GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);
            return false;
        }
        GDALRasterBandH bRe  = GDALGetRasterBand(hBlk, 1);
        GDALRasterBandH bIm  = GDALGetRasterBand(hBlk, 2);
        GDALRasterBandH bCoh = GDALGetRasterBand(hBlk, 3);
        const int blkH = GDALGetRasterYSize(hBlk);

        const int outStart = discardTop[b] / azLooks;                       // 与旧管线行为一致 (floor)
        const int outCount = (L - discardTop[b] - discardBottom[b]) / azLooks;
        const Poly& P = corr[b];

        for (int k = 0; k < outCount; ++k) {
            const int j = outStart + k;                                     // 块内行号
            if (j >= blkH) break;
            const int outRow = burstOutOffsets[b] + k;

            GDALRasterIO(bRe,  GF_Read, 0, j, outW, 1, rowRe.data(),  outW, 1, GDT_Float32, 0, 0);
            GDALRasterIO(bIm,  GF_Read, 0, j, outW, 1, rowIm.data(),  outW, 1, GDT_Float32, 0, 0);
            GDALRasterIO(bCoh, GF_Read, 0, j, outW, 1, rowCoh.data(), outW, 1, GDT_Float32, 0, 0);

            const double ph = P.a + P.b * j + P.c * j * j;   // 校正相位 (块行 j)
            if (std::abs(ph) > 1e-9) {
                const float c = std::cos(static_cast<float>(ph));
                const float s = std::sin(static_cast<float>(ph));
                for (int col = 0; col < outW; ++col) {
                    const float re = rowRe[col];
                    const float im = rowIm[col];
                    rowComplex[col] = std::complex<float>(re * c - im * s, im * c + re * s);
                    rowPhase[col] = std::atan2(rowComplex[col].imag(), rowComplex[col].real());
                }
            } else {
                for (int col = 0; col < outW; ++col) {
                    rowComplex[col] = std::complex<float>(rowRe[col], rowIm[col]);
                    rowPhase[col] = std::atan2(rowIm[col], rowRe[col]);
                }
            }

            GDALRasterIO(GDALGetRasterBand(hIfg,1), GF_Write, 0, outRow, outW, 1, reinterpret_cast<CFloat32*>(rowComplex.data()), outW, 1, GDT_CFloat32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(hCoh,1), GF_Write, 0, outRow, outW, 1, rowCoh.data(), outW, 1, GDT_Float32, 0, 0);
            GDALRasterIO(GDALGetRasterBand(hPh,1),  GF_Write, 0, outRow, outW, 1, rowPhase.data(), outW, 1, GDT_Float32, 0, 0);

            if (k == 0 && havePrev) {
                // ── 校正后边界连续性诊断 ──
                double sumRe = 0, sumIm = 0, sumW = 0;
                for (int col = 0; col < outW; ++col) {
                    double w = std::min(prevCoh[col], rowCoh[col]);
                    if (w < 0.05) continue;
                    double dp = rowPhase[col] - prevPhase[col];
                    sumRe += w * std::cos(dp);
                    sumIm += w * std::sin(dp);
                    sumW += w;
                }
                double meanDp = std::atan2(sumIm, sumRe);
                double stdDp = 0;
                if (sumW > 0) {
                    double rr = std::sqrt(sumRe * sumRe + sumIm * sumIm) / sumW;
                    stdDp = std::sqrt(std::max(0.0, -2.0 * std::log(std::max(1e-12, rr))));
                }
                double cohMean = 0; int cohN = 0;
                for (int col = 0; col < outW; ++col) {
                    if (prevCoh[col] > 0.05 && rowCoh[col] > 0.05) {
                        cohMean += 0.5 * (prevCoh[col] + rowCoh[col]); ++cohN;
                    }
                }
                if (cohN > 0) cohMean /= cohN;
                seamPhaseMean.append(meanDp);
                seamPhaseStd.append(stdDp);
                seamCohMean.append(cohMean);
                qDebug().nospace() << "[Deburst] seam " << (b) << "->" << (b + 1)
                    << " meanDp=" << (meanDp * 180.0 / M_PI) << "deg"
                    << " stdDp=" << (stdDp * 180.0 / M_PI) << "deg"
                    << " coh=" << cohMean;
            }
            if (k == outCount - 1) {
                prevPhase = rowPhase;
                prevCoh = rowCoh;
                havePrev = true;
            }
        }

        GDALClose(hBlk);
        qDebug() << "[Deburst] burst" << (b+1) << "/" << N << "done";
    }

    GDALClose(hIfg); GDALClose(hCoh); GDALClose(hPh);

    // ── 清理临时块 ──
    for (int b = 0; b < N; ++b) {
        QString blkPath = ctx.burstBlockBase + QStringLiteral("_b%1.tif").arg(b + 1);
        QFile::remove(blkPath);
    }

    // 边界汇总
    if (!seamPhaseMean.isEmpty()) {
        double m = 0, s = 0, c = 0;
        for (int i = 0; i < seamPhaseMean.size(); ++i) {
            m += seamPhaseMean[i]; s += seamPhaseStd[i]; c += seamCohMean[i];
        }
        m /= seamPhaseMean.size(); s /= seamPhaseMean.size(); c /= seamPhaseMean.size();
        qDebug().nospace() << "[Deburst] SUMMARY mode="
            << (mode == Mode::Empirical ? "empirical"
                : mode == Mode::Annotation ? "annotation" : "off")
            << " seams=" << seamPhaseMean.size()
            << " mean|dPhi|=" << (std::abs(m) * 180.0 / M_PI) << "deg"
            << " meanStd=" << (s * 180.0 / M_PI) << "deg"
            << " meanCoh=" << c;
    }

    ctx.outWidth  = outW;
    ctx.outHeight = outH;
    qDebug() << "[Deburst] SUCCESS out=" << outW << "x" << outH;
    return true;
}
