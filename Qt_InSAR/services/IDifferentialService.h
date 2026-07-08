#ifndef IDIFFERENTIALSERVICE_H
#define IDIFFERENTIALSERVICE_H

#include "IProcessingService.h"
#include "domain/params/DifferentialParams.h"

class IDifferentialService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IDifferentialService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const DifferentialParams& params) = 0;
    virtual DifferentialParams params() const = 0;
};

#endif // IDIFFERENTIALSERVICE_H
