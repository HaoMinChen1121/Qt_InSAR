#ifndef IFLATEARTHSERVICE_H
#define IFLATEARTHSERVICE_H

#include "IProcessingService.h"
#include "domain/params/FlatEarthParams.h"

class IFlatEarthService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IFlatEarthService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const FlatEarthParams& params) = 0;
    virtual FlatEarthParams params() const = 0;
};

#endif // IFLATEARTHSERVICE_H
