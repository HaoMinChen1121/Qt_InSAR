#include "GeocodingServiceImpl.h"

GeocodingServiceImpl::GeocodingServiceImpl(QObject* parent)
    : IGeocodingService(parent) {}

void GeocodingServiceImpl::setParams(const GeocodingParams& params) { mParams = params; }
GeocodingParams GeocodingServiceImpl::params() const { return mParams; }

void GeocodingServiceImpl::execute()
{
    // TODO: 地理编码算法实现
    emit progressChanged(50, tr("地理编码中..."));
    emit finished(true, mParams.outputDir + "/" + mParams.outputPrefix);
}

void GeocodingServiceImpl::cancel() { mRunning = false; }
bool GeocodingServiceImpl::isRunning() const { return mRunning; }
