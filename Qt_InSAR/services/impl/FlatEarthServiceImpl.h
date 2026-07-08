#ifndef FLATEARTHSERVICEIMPL_H
#define FLATEARTHSERVICEIMPL_H

#include "services/IFlatEarthService.h"

class FlatEarthServiceImpl : public IFlatEarthService
{
    Q_OBJECT
public:
    explicit FlatEarthServiceImpl(QObject* parent = nullptr);
    void setParams(const FlatEarthParams& params) override;
    FlatEarthParams params() const override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    FlatEarthParams mParams;
    bool mRunning = false;
};

#endif // FLATEARTHSERVICEIMPL_H
