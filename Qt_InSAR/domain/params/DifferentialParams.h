#ifndef DIFFERENTIALPARAMS_H
#define DIFFERENTIALPARAMS_H

#include <QString>

struct DifferentialParams
{
    QString   demPath;
    QString   displacementDirection = "LOS";  // "LOS" / "Vertical"
    bool      atmosphericCorrection = false;
    QString   atmosphericModel = "Linear";    // "Linear" / "PowerLaw"
    bool      topographicCorrection = true;   // 地形相位去除
};

#endif // DIFFERENTIALPARAMS_H
