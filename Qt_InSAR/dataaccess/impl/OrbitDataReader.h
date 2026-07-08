#ifndef ORBITDATAREADER_H
#define ORBITDATAREADER_H

#include "dataaccess/IOrbitDataReader.h"

class OrbitDataReader : public IOrbitDataReader
{
public:
    OrbitDataReader();
    ~OrbitDataReader() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;

    OrbitInfo readOrbit() override;
    QVector<OrbitStateVector> interpolate(
        double startTime, double endTime, double interval) override;

private:
    QString mFilePath;
    OrbitInfo mOrbit;
    bool mOpen = false;
};

#endif // ORBITDATAREADER_H
