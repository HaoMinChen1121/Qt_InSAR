#include "WorkerManager.h"
#include "TaskWorker.h"
#include "services/IProcessingService.h"
#include <QThread>

WorkerManager::WorkerManager(QObject* parent) : QObject(parent) {}
WorkerManager::~WorkerManager() { cancelAll(); }

void WorkerManager::enqueueTask(IProcessingService* service)
{
    mQueue.enqueue(service);
    if (!mBusy) processNext();
}

void WorkerManager::cancelAll()
{
    mQueue.clear();
        if (mCurrentWorker)
    {
        mCurrentWorker->requestCancel();
                if (mThread && mThread->isRunning())
        {
            mThread->quit();
            mThread->wait(3000);
        }
    }
}

int WorkerManager::pendingCount() const { return mQueue.size(); }

void WorkerManager::processNext()
{
        if (mQueue.isEmpty())
    {
        mBusy = false;
        emit allTasksCompleted();
        return;
    }
    mBusy = true;
    IProcessingService* svc = mQueue.dequeue();
    mThread = new QThread(this);
    mCurrentWorker = new TaskWorker(svc);
    mCurrentWorker->moveToThread(mThread);

    connect(mThread, &QThread::started, mCurrentWorker, &TaskWorker::process);
    connect(mCurrentWorker, &TaskWorker::finished, this, [this]() {
        mThread->quit();
        mThread->wait();
        mCurrentWorker->deleteLater();
        mThread->deleteLater();
        mCurrentWorker = nullptr;
        mThread = nullptr;
        emit taskFinished();
        processNext();
    });
    connect(mCurrentWorker, &TaskWorker::progressChanged,
        this, &WorkerManager::taskProgressChanged);
    connect(mCurrentWorker, &TaskWorker::errorOccurred,
        this, &WorkerManager::taskError);

    mThread->start();
}
