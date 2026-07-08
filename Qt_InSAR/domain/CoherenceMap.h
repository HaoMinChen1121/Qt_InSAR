#ifndef COHERENCEMAP_H
#define COHERENCEMAP_H

#include <QString>
#include <QSize>

struct CoherenceMap
{
    QString   layerId;
    QString   displayName;
    QString   filePath;
    QSize     rasterSize;
    double    meanCoherence = 0;
    bool      visible = true;
};

#endif // COHERENCEMAP_H
