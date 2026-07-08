#ifndef IORBITDATAREADER_H
#define IORBITDATAREADER_H

#include <QString>
#include <QVector>
#include "domain/OrbitInfo.h"

class IOrbitDataReader
{
public:
    virtual ~IOrbitDataReader() = default;

    virtual bool open(const QString& filePath) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual OrbitInfo readOrbit() = 0;
    virtual QVector<OrbitStateVector> interpolate(
        double startTime, double endTime, double interval) = 0;
};

#endif // IORBITDATAREADER_H
