#ifndef IPROCESSINGSERVICE_H
#define IPROCESSINGSERVICE_H

#include <QObject>
#include <QString>

class IProcessingService : public QObject
{
    Q_OBJECT
public:
    explicit IProcessingService(QObject* parent = nullptr) : QObject(parent) {}

    virtual void execute() = 0;
    virtual void cancel() = 0;
    virtual bool isRunning() const = 0;

signals:
    void progressChanged(int percent, const QString& statusMsg);
    void finished(bool success, const QString& outputPath);
    void errorOccurred(const QString& errorMsg);
};

#endif // IPROCESSINGSERVICE_H
