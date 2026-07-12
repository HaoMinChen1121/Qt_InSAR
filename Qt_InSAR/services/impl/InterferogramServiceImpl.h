#ifndef INTERFEROGRAMSERVICEIMPL_H
#define INTERFEROGRAMSERVICEIMPL_H

#include "services/IInterferogramService.h"
#include "domain/QsarProduct.h"

#include <QVector>
#include <complex>

class InterferogramServiceImpl : public IInterferogramService
{
    Q_OBJECT
public:
    explicit InterferogramServiceImpl(QObject* parent = nullptr);
    void setParams(const InterferogramParams& params) override;
    InterferogramParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    // Stage 1: 多视+干涉+相干性
    bool stageInterferogram(
        const QString& masterPath, const QString& slavePath,
        const QString& outPath, int width, int height,
        int rgLooks, int azLooks);

    // Stage 2: 平地相位去除 (椭球面)
    bool stageFlatEarth(
        const QString& ifgPath, const QString& outPath,
        int width, int height, double wavelength,
        double nearRange, double rangeSpacing, double prf);

    // Stage 3: 差分干涉 (DEM)
    bool stageDifferential(
        const QString& flatPath, const QString& demPath, const QString& outPath,
        int width, int height, double wavelength,
        double nearRange, double rangeSpacing);

    InterferogramParams mParams;
    bool mRunning = false;
    bool mCancelled = false;
};

#endif // INTERFEROGRAMSERVICEIMPL_H
