#ifndef IFILTERSERVICE_H
#define IFILTERSERVICE_H

#include "IProcessingService.h"
#include "domain/params/FilterParams.h"

class IFilterService : public IProcessingService
{
    Q_OBJECT
public:
    explicit IFilterService(QObject* parent = nullptr) : IProcessingService(parent) {}

    virtual void setParams(const FilterParams& params) = 0;
    virtual FilterParams params() const = 0;
    virtual void preview() = 0;

signals:
    void previewReady(const QString& previewPath);
};

#endif // IFILTERSERVICE_H
