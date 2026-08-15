#ifndef FILTERSERVICEIMPL_H
#define FILTERSERVICEIMPL_H

#include "services/IFilterService.h"

class ProductManager;

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

    // Product 驱动: 输入经 ProductManager 按 productId 解析
    void setProductManager(ProductManager* pm) { mProductManager = pm; }

private:
    FilterParams mParams;
    bool mRunning = false;
    ProductManager* mProductManager = nullptr;
};

#endif // FILTERSERVICEIMPL_H
