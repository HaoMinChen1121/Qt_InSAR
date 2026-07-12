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
        GdalSlcReader* reader,
        const QString& masterPath, const QString& slavePath,
        int masterW, int masterH, int slaveW, int slaveH,
        int windowSize, int searchWindow, double corrThreshold);

    RegPolynomial fineRegister(
        QVector<CoarseGcp>& gcps,
        GdalSlcReader* reader,
        const QString& masterPath, const QString& slavePath,
        int masterW, int masterH, int slaveW, int slaveH,
        int oversampleFactor, int polyDegree, double corrThreshold,
        int windowSize);

    bool resampleImage(
        const RegPolynomial& poly,
        GdalSlcReader* reader, GdalSlcWriter* writer,
        const QString& masterPath, const QString& slavePath,
        int masterW, int masterH, int slaveW, int slaveH,
        const QString& interpMethod, int sincWindow, double sincBeta,
        const QString& outputPath);

    // ── 波段对配准 ──
    bool processBandPair(
        const SarBandDescriptor& masterBand,
        const SarBandDescriptor& slaveBand,
        const QString& outputDir, const QString& prefix,
        int pairIndex, int totalPairs);

    // ── 辅助 ──
    std::complex<float> interp2D(
        const QVector<std::complex<float>>& data,
        int width, int height, double x, double y,
        const QString& method, int sincWindow, double sincBeta);

    double meanCorrelation(const QVector<CoarseGcp>& gcps) const;

    RegistrationParams mParams;
    double mCorrelation = 0.0;
    bool mRunning = false;
    bool mCancelled = false;
};

#endif // REGISTRATIONSERVICEIMPL_H
