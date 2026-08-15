#include "UnwrappingServiceImpl.h"

UnwrappingServiceImpl::UnwrappingServiceImpl(QObject* parent)
    : IUnwrappingService(parent) {}

void UnwrappingServiceImpl::setParams(const UnwrappingParams& params) { mParams = params; }
UnwrappingParams UnwrappingServiceImpl::params() const { return mParams; }

void UnwrappingServiceImpl::execute()
{
    // TODO: 相位解缠算法实现 (质量引导区域增长, Phase 2)
    emit progressChanged(50, tr("相位解缠中..."));
    emit finished(true, tr("(占位) Phase 2 实现后产出 unwrap/ 产品"));
}

void UnwrappingServiceImpl::cancel() { mRunning = false; }
bool UnwrappingServiceImpl::isRunning() const { return mRunning; }
