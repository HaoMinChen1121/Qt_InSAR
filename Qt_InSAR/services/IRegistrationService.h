#ifndef IREGISTRATIONSERVICE_H
#define IREGISTRATIONSERVICE_H

#include "IProcessingService.h"
#include "domain/params/RegistrationParams.h"

class IRegistrationService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IRegistrationService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const RegistrationParams& params) = 0;
    virtual RegistrationParams params() const = 0;
    virtual double currentCorrelation() const = 0;
};

#endif // IREGISTRATIONSERVICE_H
