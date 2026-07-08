#include "FlatEarthServiceImpl.h"

FlatEarthServiceImpl::FlatEarthServiceImpl(QObject* parent)
    : IFlatEarthService(parent) {}

void FlatEarthServiceImpl::setParams(const FlatEarthParams& params) { mParams = params; }
FlatEarthParams FlatEarthServiceImpl::params() const { return mParams; }

void FlatEarthServiceImpl::execute()
{
    emit progressChanged(50, tr("去平地效应中..."));
    emit finished(true, QString());
}

void FlatEarthServiceImpl::cancel() { mRunning = false; }
bool FlatEarthServiceImpl::isRunning() const { return mRunning; }
