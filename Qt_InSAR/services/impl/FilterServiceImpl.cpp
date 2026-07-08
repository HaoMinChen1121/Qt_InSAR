#include "FilterServiceImpl.h"

FilterServiceImpl::FilterServiceImpl(QObject* parent)
    : IFilterService(parent) {}

void FilterServiceImpl::setParams(const FilterParams& params) { mParams = params; }
FilterParams FilterServiceImpl::params() const { return mParams; }

void FilterServiceImpl::preview()
{
    // TODO: 滤波预览
    emit previewReady(QString());
}

void FilterServiceImpl::execute()
{
    // TODO: 滤波算法实现 (Goldstein/Baran)
    emit progressChanged(50, tr("干涉图滤波中..."));
    emit finished(true, QString());
}

void FilterServiceImpl::cancel() { mRunning = false; }
bool FilterServiceImpl::isRunning() const { return mRunning; }
