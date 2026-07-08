#ifndef REGISTRATIONSERVICEIMPL_H
#define REGISTRATIONSERVICEIMPL_H

#include "services/IRegistrationService.h"

class RegistrationServiceImpl : public IRegistrationService
{
    Q_OBJECT
public:
    explicit RegistrationServiceImpl(QObject* parent = nullptr);

    void setParams(const RegistrationParams& params) override;
    RegistrationParams params() const override;
    double currentCorrelation() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    RegistrationParams mParams;
    bool mRunning = false;
};

#endif // REGISTRATIONSERVICEIMPL_H
