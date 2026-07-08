#include "DifferentialServiceImpl.h"

DifferentialServiceImpl::DifferentialServiceImpl(QObject* parent)
    : IDifferentialService(parent) {}

void DifferentialServiceImpl::setParams(const DifferentialParams& params) { mParams = params; }
DifferentialParams DifferentialServiceImpl::params() const { return mParams; }

void DifferentialServiceImpl::execute()
{
    emit progressChanged(50, tr("差分干涉中..."));
    emit finished(true, QString());
}

void DifferentialServiceImpl::cancel() { mRunning = false; }
bool DifferentialServiceImpl::isRunning() const { return mRunning; }
