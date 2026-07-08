#include "TaskWorker.h"
#include "services/IProcessingService.h"

TaskWorker::TaskWorker(IProcessingService* service, QObject* parent)
    : QObject(parent), mService(service) {}

void TaskWorker::process()
{
        if (!mService)
    {
        emit errorOccurred(tr("Service is null"));
        return;
    }
    connect(mService, &IProcessingService::progressChanged,
        this, &TaskWorker::progressChanged);
    connect(mService, &IProcessingService::errorOccurred,
        this, &TaskWorker::errorOccurred);
    mService->execute();
    emit finished();
}

void TaskWorker::requestCancel()
{
    mCancelled = true;
    if (mService) mService->cancel();
}
