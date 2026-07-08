#ifndef DEFORMATIONMAP_H
#define DEFORMATIONMAP_H

#include <QString>
#include <QSize>

struct DeformationMap
{
    QString   layerId;
    QString   displayName;
    QString   filePath;
    QSize     rasterSize;
    QString   direction;       // "LOS" / "Vertical" / "Horizontal"
    double    minDeformation = 0; // (m)
    double    maxDeformation = 0; // (m)
    bool      visible = true;
};

#endif // DEFORMATIONMAP_H
