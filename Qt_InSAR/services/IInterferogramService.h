#ifndef IINTERFEROGRAMSERVICE_H
#define IINTERFEROGRAMSERVICE_H

#include "IProcessingService.h"
#include "domain/params/InterferogramParams.h"

class IInterferogramService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IInterferogramService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const InterferogramParams& params) = 0;
    virtual InterferogramParams params() const = 0;
};

#endif // IINTERFEROGRAMSERVICE_H
