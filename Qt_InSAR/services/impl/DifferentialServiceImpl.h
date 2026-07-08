#ifndef DIFFERENTIALSERVICEIMPL_H
#define DIFFERENTIALSERVICEIMPL_H

#include "services/IDifferentialService.h"

class DifferentialServiceImpl : public IDifferentialService
{
    Q_OBJECT
public:
    explicit DifferentialServiceImpl(QObject* parent = nullptr);
    void setParams(const DifferentialParams& params) override;
    DifferentialParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    DifferentialParams mParams;
    bool mRunning = false;
};

#endif // DIFFERENTIALSERVICEIMPL_H
