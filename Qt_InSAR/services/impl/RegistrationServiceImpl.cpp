#include "RegistrationServiceImpl.h"

RegistrationServiceImpl::RegistrationServiceImpl(QObject* parent)
    : IRegistrationService(parent) {}

void RegistrationServiceImpl::setParams(const RegistrationParams& params) { mParams = params; }
RegistrationParams RegistrationServiceImpl::params() const { return mParams; }
double RegistrationServiceImpl::currentCorrelation() const { return 0.0; }

void RegistrationServiceImpl::execute()
{
    // TODO: 配准算法实现
    emit progressChanged(50, tr("配准处理中..."));
    emit finished(true, mParams.outputDir + "/" + mParams.outputPrefix);
}

void RegistrationServiceImpl::cancel() { mRunning = false; }
bool RegistrationServiceImpl::isRunning() const { return mRunning; }
