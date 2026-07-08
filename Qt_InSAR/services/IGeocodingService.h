#ifndef IGEOCODINGSERVICE_H
#define IGEOCODINGSERVICE_H

#include "IProcessingService.h"
#include "domain/params/GeocodingParams.h"

class IGeocodingService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IGeocodingService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const GeocodingParams& params) = 0;
    virtual GeocodingParams params() const = 0;
};

#endif // IGEOCODINGSERVICE_H
