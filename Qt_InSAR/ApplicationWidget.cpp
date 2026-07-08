#include "ApplicationWidget.h"
#include "SARibbonMainWindow.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QApplication>

ApplicationWidget::ApplicationWidget(QWidget* parent) : SARibbonApplicationWidget(qobject_cast<SARibbonMainWindow*>(parent))
{
    setupUI();
}

void ApplicationWidget::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    QPushButton* openBtn = new QPushButton(QStringLiteral("打开项目..."), this);
    QPushButton* saveBtn = new QPushButton(QStringLiteral("保存项目"), this);
    QPushButton* exitBtn = new QPushButton(QStringLiteral("退出"), this);

        for (auto* btn : { openBtn, saveBtn, exitBtn })
    {
        btn->setFlat(true);
        btn->setMinimumHeight(32);
        btn->setStyleSheet(
            "QPushButton { text-align: left; padding: 6px 16px; border: none; }"
            "QPushButton:hover { background-color: #0078D7; color: white; }");
    }

    layout->addWidget(openBtn);
    layout->addWidget(saveBtn);
    layout->addWidget(exitBtn);
    layout->addStretch();

    connect(openBtn, &QPushButton::clicked, this, &ApplicationWidget::openRequested);
    connect(saveBtn, &QPushButton::clicked, this, &ApplicationWidget::saveRequested);
    connect(exitBtn, &QPushButton::clicked, this, []() { qApp->quit(); });
    connect(exitBtn, &QPushButton::clicked, this, &ApplicationWidget::exitRequested);

    setMinimumWidth(200);
    setMaximumWidth(280);
}
