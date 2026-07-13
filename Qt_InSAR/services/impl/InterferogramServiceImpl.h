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
    bool stageInterferogram(
        const QString& masterPath, const QString& slavePath,
        const QString& outBase, int width, int height,
        int rgLooks, int azLooks);

    bool stageFlatEarth(
        const QString& ifgPath, const QString& outBase,
        int width, int height, double wavelength,
        double nearRange, double rangeSpacing, double prf,
        double incidenceAngleRad, double Bpar);

    bool stageDifferential(
        const QString& flatPath, const QString& demPath, const QString& outBase,
        int width, int height, double wavelength,
        double nearRange, double rangeSpacing,
        double incidenceAngleRad, double Bperp);

    InterferogramParams mParams;
    bool mRunning = false;
    bool mCancelled = false;
};

#endif // INTERFEROGRAMSERVICEIMPL_H
