#include "InterferogramServiceImpl.h"

InterferogramServiceImpl::InterferogramServiceImpl(QObject* parent)
    : IInterferogramService(parent) {}

void InterferogramServiceImpl::setParams(const InterferogramParams& params) { mParams = params; }
InterferogramParams InterferogramServiceImpl::params() const { return mParams; }

void InterferogramServiceImpl::execute()
{
    // TODO: 干涉图生成算法
    emit progressChanged(50, tr("干涉图生成中..."));
    emit finished(true, mParams.outputDir + "/" + mParams.outputPrefix);
}

void InterferogramServiceImpl::cancel() { mRunning = false; }
bool InterferogramServiceImpl::isRunning() const { return mRunning; }
