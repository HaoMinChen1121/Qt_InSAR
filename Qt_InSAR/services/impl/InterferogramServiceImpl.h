#ifndef INTERFEROGRAMSERVICEIMPL_H
#define INTERFEROGRAMSERVICEIMPL_H

#include "services/IInterferogramService.h"

class InterferogramServiceImpl : public IInterferogramService
{
    Q_OBJECT
public:
    explicit InterferogramServiceImpl(QObject* parent = nullptr);
    void setParams(const InterferogramParams& params) override;
    InterferogramParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    InterferogramParams mParams;
    bool mRunning = false;
};

#endif // INTERFEROGRAMSERVICEIMPL_H
