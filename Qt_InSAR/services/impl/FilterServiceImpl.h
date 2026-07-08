#ifndef FILTERSERVICEIMPL_H
#define FILTERSERVICEIMPL_H

#include "services/IFilterService.h"

class FilterServiceImpl : public IFilterService
{
    Q_OBJECT
public:
    explicit FilterServiceImpl(QObject* parent = nullptr);
    void setParams(const FilterParams& params) override;
    FilterParams params() const override;
    void preview() override;
    void execute() override;
    void cancel() override;
    bool isRunning() const override;

private:
    FilterParams mParams;
    bool mRunning = false;
};

#endif // FILTERSERVICEIMPL_H
