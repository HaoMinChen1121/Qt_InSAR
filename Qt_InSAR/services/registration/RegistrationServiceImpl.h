#ifndef REGISTRATIONSERVICEIMPL_H
#define REGISTRATIONSERVICEIMPL_H

#include "IRegistrationService.h"
#include <memory>

class RegistrationServiceImpl : public IRegistrationService
{
    Q_OBJECT
public:
    explicit RegistrationServiceImpl(QObject* parent = nullptr);

    void setParams(const RegistrationParams& params) override;
    RegistrationParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    RegistrationParams mParams;
    bool mRunning   = false;
    bool mCancelled = false;
};

#endif // REGISTRATIONSERVICEIMPL_H
