#ifndef UNWRAPPINGSERVICEIMPL_H
#define UNWRAPPINGSERVICEIMPL_H

#include "services/IUnwrappingService.h"

class UnwrappingServiceImpl : public IUnwrappingService
{
    Q_OBJECT
public:
    explicit UnwrappingServiceImpl(QObject* parent = nullptr);
    void setParams(const UnwrappingParams& params) override;
    UnwrappingParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    UnwrappingParams mParams;
    bool mRunning = false;
};

#endif // UNWRAPPINGSERVICEIMPL_H
