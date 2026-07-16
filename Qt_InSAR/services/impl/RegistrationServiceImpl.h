#ifndef REGISTRATIONSERVICEIMPL_H
#define REGISTRATIONSERVICEIMPL_H

#include "services/IRegistrationService.h"
#include "domain/BaselineInfo.h"
#include "dataaccess/ISarProduct.h"

#include <QVector>
#include <complex>

class GdalSlcReader;
class GdalSlcWriter;

class RegistrationServiceImpl : public IRegistrationService
{
    Q_OBJECT
public:
    explicit RegistrationServiceImpl(QObject* parent = nullptr);

    void setParams(const RegistrationParams& params) override;
    RegistrationParams params() const override;
    double currentCorrelation() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    // ── 内部数据结构 ──
    struct CoarseGcp {
        int    row, col;        // 主影像坐标
        double rangeOff;        // 距离向偏移 (像素)
        double aziOff;          // 方位向偏移 (像素)
        double correlation;     // 相关系数
    };

    struct RegPolynomial {
        double rangeCoeffs[6];  // Δr = a0 + a1*r + a2*a + a3*r*a + a4*r² + a5*a²
        double aziCoeffs[6];    // Δa = b0 + b1*r + b2*a + b3*r*a + b4*r² + b5*a²
        double rmseRange;
        double rmseAzimuth;
        int    validGcps;
        // ESD per-burst 方位向修正 (TOPSAR)
        QVector<double> burstAziCorrections; // 每个 burst 的绝对修正量 (像素, 已中心化)
        QVector<int>    burstStartLines;      // 每个 burst 在拼接影像中的起行号
        int             linesPerBurst = 0;    // 每个 burst 的有效行数
    };

    // 逐 burst 局部多项式 (TOPSAR per-burst independent registration)
    struct BurstLocalPoly {
        double rangeCoeffs[6];
        double aziCoeffs[6];
        int    masterStartLine = 0;  // burst 在主影像中的起始行
        int    masterEndLine   = 0;  // burst 在主影像中的结束行 (不含)
    };

    // ── 算法模块 ──
    BaselineInfo estimateBaseline(
        const QList<OrbitStateVector>& masterOrbits,
        const QList<OrbitStateVector>& slaveOrbits,
        double wavelength, double nearRange, double rangeSpacing,
        int rangeSamples);

    QVector<CoarseGcp> coarseByOrbit(
        const QList<OrbitStateVector>& mOrb,
        const QList<OrbitStateVector>& sOrb,
        const DopplerInfo& mDop, const DopplerInfo& sDop,
        double nearRange, double rangeSpacing, double aziSpacing,
        double prf, int width, int height, int numGcp);

    void coarseByCorrelation(
        QVector<CoarseGcp>& gcps,
        GdalSlcReader* mReader, GdalSlcReader* sReader,
        int windowSize, int searchWindow);

    // ── FFTW3 幅度域粗配准 ──
    void coarseByFFT(
        QVector<CoarseGcp>& gcps,
        GdalSlcReader* mReader, GdalSlcReader* sReader,
        int masterW, int masterH, int slaveW, int slaveH,
        int windowSize);

    // ── FFTW3 复数域精配准 ──
    void fftFineRefine(
        QVector<CoarseGcp>& gcps,
        GdalSlcReader* mReader, GdalSlcReader* sReader,
        int masterW, int masterH, int slaveW, int slaveH,
        int windowSize);

    RegPolynomial fineRegister(
        QVector<CoarseGcp>& gcps,
        GdalSlcReader* mReader, GdalSlcReader* sReader,
        int masterW, int masterH, double corrThreshold,
        int windowSize);

    bool resampleImage(
        const RegPolynomial& poly,
        GdalSlcReader* sReader, GdalSlcWriter* writer,
        int masterW, int masterH, int slaveW, int slaveH,
        const QString& interpMethod, int sincWindow, double sincBeta,
        const QString& outputPath);

    // ── ESD 方位向精化 ──
    void esdRefine(RegPolynomial& poly,
        GdalSlcReader* mReader, GdalSlcReader* sReader,
        const SarBandDescriptor& masterBand,
        const SarBandDescriptor& slaveBand,
        int masterW, int masterH);

    // ── 逐 burst 局部多项式拟合 (TOPSAR) ──
    QVector<BurstLocalPoly> fitBurstLocalPolynomials(
        const QVector<CoarseGcp>& gcps,
        const RegPolynomial& globalPoly,
        const SarBandDescriptor& masterBand,
        int masterW, int masterH);

    // ── 逐 burst 独立重采样 + 拼接 ──
    bool resampleImagePerBurst(
        const QVector<BurstLocalPoly>& burstPolys,
        GdalSlcReader* sReader, GdalSlcWriter* writer,
        int masterW, int masterH, int slaveW, int slaveH,
        const QString& interpMethod, int sincWindow, double sincBeta,
        const QString& outputPath);

    // ── 波段对配准 ──
    bool processBandPair(
        const SarBandDescriptor& masterBand,
        const SarBandDescriptor& slaveBand,
        const QString& outputDir, const QString& prefix,
        int pairIndex);

    // ── 辅助 ──
    std::complex<float> interp2D(
        const QVector<std::complex<float>>& data,
        int width, int height, double x, double y,
        const QString& method, int sincWindow, double sincBeta);

    double meanCorrelation(const QVector<CoarseGcp>& gcps) const;

    // 从 GCP 集合拟合 2 阶多项式 (供逐 burst 局部拟合使用)
    static bool fitPolynomial(const QVector<CoarseGcp>& gcps,
                              int masterW, int masterH,
                              double* rCoeffs, double* aCoeffs);

    RegistrationParams mParams;
    double mCorrelation = 0.0;
    bool mRunning = false;
    bool mCancelled = false;
};

#endif // REGISTRATIONSERVICEIMPL_H
