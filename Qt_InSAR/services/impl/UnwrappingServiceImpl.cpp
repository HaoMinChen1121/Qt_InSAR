#include "UnwrappingServiceImpl.h"

UnwrappingServiceImpl::UnwrappingServiceImpl(QObject* parent)
    : IUnwrappingService(parent) {}

void UnwrappingServiceImpl::setParams(const UnwrappingParams& params) { mParams = params; }
UnwrappingParams UnwrappingServiceImpl::params() const { return mParams; }

void UnwrappingServiceImpl::execute()
{
    // TODO: 相位解缠算法实现 (枝切法/最小二乘法)
    emit progressChanged(50, tr("相位解缠中..."));
    emit finished(true, QString());
}

void UnwrappingServiceImpl::cancel() { mRunning = false; }
bool UnwrappingServiceImpl::isRunning() const { return mRunning; }
