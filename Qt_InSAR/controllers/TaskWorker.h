#ifndef TASKWORKER_H
#define TASKWORKER_H

#include <QObject>
#include <QThread>

class IProcessingService;

class TaskWorker : public QObject
{
    Q_OBJECT
public:
    explicit TaskWorker(IProcessingService* service, QObject* parent = nullptr);

public slots:
    void process();
    void requestCancel();

signals:
    void finished();
    void progressChanged(int percent, const QString& msg);
    void errorOccurred(const QString& errorMsg);

private:
    IProcessingService* mService;
    bool mCancelled = false;
};

#endif // TASKWORKER_H
