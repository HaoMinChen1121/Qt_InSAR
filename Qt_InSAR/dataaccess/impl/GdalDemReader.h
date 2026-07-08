#ifndef GDALDEMREADER_H
#define GDALDEMREADER_H

#include "dataaccess/IDemReader.h"

class GdalDemReader : public IDemReader
{
public:
    GdalDemReader();
    ~GdalDemReader() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;

    QVector<float> readElevation() override;
    QVector<float> readElevationWindow(
        int colStart, int rowStart, int colSize, int rowSize) override;

    int     width() const override;
    int     height() const override;
    double* geoTransform() const override;
    QString projection() const override;
    double  noDataValue() const override;

private:
    void* mDataset = nullptr;
    int mWidth = 0, mHeight = 0;
    double mGeoTransform[6] = {};
    double mNoData = -9999;
};

#endif // GDALDEMREADER_H
