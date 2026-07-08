#ifndef IUNWRAPPINGSERVICE_H
#define IUNWRAPPINGSERVICE_H

#include "IProcessingService.h"
#include "domain/params/UnwrappingParams.h"

class IUnwrappingService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IUnwrappingService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const UnwrappingParams& params) = 0;
    virtual UnwrappingParams params() const = 0;
};

#endif // IUNWRAPPINGSERVICE_H
