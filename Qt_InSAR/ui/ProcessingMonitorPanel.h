#ifndef PROCESSINGMONITORPANEL_H
#define PROCESSINGMONITORPANEL_H

#include <QWidget>

class QPlainTextEdit;
class QProgressBar;
class QLabel;
class QToolBar;

class ProcessingMonitorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ProcessingMonitorPanel(QWidget* parent = nullptr);

public slots:
    void onProgress(int percent, const QString& statusMsg);
    void onFinished(bool success, const QString& outputPath);
    void onError(const QString& errorMsg);
    void clearLog();
    void exportLog();
    void appendLog(const QString& msg, const QString& color);

private:
    QPlainTextEdit* mLog;
    QProgressBar* mProgress;
    QLabel* mStatusLabel;
};

#endif // PROCESSINGMONITORPANEL_H
