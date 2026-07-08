#include "ProcessingMonitorPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QLabel>
#include <QToolBar>
#include <QDateTime>
#include <QTextStream>
#include <QFileDialog>
#include <QFile>

ProcessingMonitorPanel::ProcessingMonitorPanel(QWidget* parent) : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    QToolBar* toolbar = new QToolBar(this);
    toolbar->addAction(tr("清空日志"), this, &ProcessingMonitorPanel::clearLog);
    toolbar->addAction(tr("导出日志"), this, &ProcessingMonitorPanel::exportLog);
    layout->addWidget(toolbar);

    mLog = new QPlainTextEdit(this);
    mLog->setReadOnly(true);
    mLog->setMaximumBlockCount(5000);
    mLog->setFont(QFont("Consolas", 9));
    layout->addWidget(mLog);

    QHBoxLayout* bottomLayout = new QHBoxLayout;
    mStatusLabel = new QLabel(tr("就绪"), this);
    mProgress = new QProgressBar(this);
    mProgress->setRange(0, 100);
    mProgress->setValue(0);
    bottomLayout->addWidget(mStatusLabel, 1);
    bottomLayout->addWidget(mProgress, 2);
    layout->addLayout(bottomLayout);
}

void ProcessingMonitorPanel::onProgress(int percent, const QString& statusMsg)
{
    mProgress->setValue(percent);
    mStatusLabel->setText(statusMsg);
    appendLog(statusMsg, "#4A90D9");
}

void ProcessingMonitorPanel::onFinished(bool success, const QString& outputPath)
{
        if (success)
    {
        appendLog(tr("处理完成: %1").arg(outputPath), "#27AE60");
        mStatusLabel->setText(tr("完成"));
        } else
    {
        appendLog(tr("处理失败"), "#E74C3C");
        mStatusLabel->setText(tr("失败"));
    }
    mProgress->setValue(success ? 100 : 0);
}

void ProcessingMonitorPanel::onError(const QString& errorMsg)
{
    appendLog(errorMsg, "#E74C3C");
    mStatusLabel->setText(tr("错误"));
}

void ProcessingMonitorPanel::appendLog(const QString& msg, const QString& color)
{
    QString timestamp = QDateTime::currentDateTime().toString("hh:mm:ss");
    mLog->appendHtml(QString("<span style='color:%1'>[%2] %3</span>")
        .arg(color, timestamp, msg));
}

void ProcessingMonitorPanel::clearLog() { mLog->clear(); }

void ProcessingMonitorPanel::exportLog()
{
    QString path = QFileDialog::getSaveFileName(this, tr("导出日志"),
        QString(), tr("文本文件 (*.txt);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(mLog->toPlainText().toUtf8());
}
