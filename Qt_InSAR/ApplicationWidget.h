#ifndef APPLICATIONWIDGET_H
#define APPLICATIONWIDGET_H

#include "SARibbonApplicationWidget.h"

class QPushButton;

class ApplicationWidget : public SARibbonApplicationWidget 
{
    Q_OBJECT
public:
    explicit ApplicationWidget(QWidget* parent = nullptr);

signals:
    void openRequested();
    void saveRequested();
    void exitRequested();

private:
    void setupUI();
};

#endif // APPLICATIONWIDGET_H
