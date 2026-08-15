#ifndef REGISTRATIONSERVICEIMPL_H
#define REGISTRATIONSERVICEIMPL_H

#include "../IRegistrationService.h"
#include <memory>
#include <vector>

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

    // 地形校正配准: 全量 DEM 内存副本 (一次加载, 并行重采样线程安全只读)
    std::vector<float> mTerrainDem;
    int  mTerrainDemW = 0;
    int  mTerrainDemH = 0;
    int  mTerrainSign = -1;
};

#endif // REGISTRATIONSERVICEIMPL_H
