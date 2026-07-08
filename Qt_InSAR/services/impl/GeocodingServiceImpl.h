#ifndef GEOCODINGSERVICEIMPL_H
#define GEOCODINGSERVICEIMPL_H

#include "services/IGeocodingService.h"

class GeocodingServiceImpl : public IGeocodingService
{
    Q_OBJECT
public:
    explicit GeocodingServiceImpl(QObject* parent = nullptr);
    void setParams(const GeocodingParams& params) override;
    GeocodingParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    GeocodingParams mParams;
    bool mRunning = false;
};

#endif // GEOCODINGSERVICEIMPL_H
