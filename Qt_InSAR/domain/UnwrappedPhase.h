#ifndef UNWRAPPEDPHASE_H
#define UNWRAPPEDPHASE_H

#include <QString>
#include <QSize>

struct UnwrappedPhase
{
    QString   layerId;
    QString   displayName;
    QString   filePath;
    QSize     rasterSize;
    QString   method;           // "BranchCut" / "LeastSquares"
    double    minValue = 0;     // 最小值 (rad)
    double    maxValue = 0;     // 最大值 (rad)
    bool      convertedToHeight = false; // 是否已转高程
    bool      visible = true;
};

#endif // UNWRAPPEDPHASE_H
