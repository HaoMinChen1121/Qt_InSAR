#ifndef WORKERMANAGER_H
#define WORKERMANAGER_H

#include <QObject>
#include <QQueue>

class QThread;
class TaskWorker;
class IProcessingService;

class WorkerManager : public QObject 
{
    Q_OBJECT
public:
    explicit WorkerManager(QObject* parent = nullptr);
    ~WorkerManager();

    void enqueueTask(IProcessingService* service);
    void cancelAll();
    int pendingCount() const;

signals:
    void taskProgressChanged(int percent, const QString& msg);
    void taskFinished();
    void taskError(const QString& errorMsg);
    void allTasksCompleted();

private:
    void processNext();

    QQueue<IProcessingService*> mQueue;
    QThread* mThread = nullptr;
    TaskWorker* mCurrentWorker = nullptr;
    bool mBusy = false;
};

#endif // WORKERMANAGER_H
