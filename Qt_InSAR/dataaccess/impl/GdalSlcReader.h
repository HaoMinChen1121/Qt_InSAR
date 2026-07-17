#ifndef GDALSLCREADER_H
#define GDALSLCREADER_H

#include "dataaccess/ISlcReader.h"
#include <complex>

class GdalSlcReader : public ISlcReader
{
public:
    GdalSlcReader();
    ~GdalSlcReader() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;

    QVector<std::complex<float>> readBand(int bandIndex = 0) override;
    QVector<std::complex<float>> readBandWindow(int bandIndex,
        int colStart, int rowStart, int colSize, int rowSize) override;

    int     width() const override;
    int     height() const override;
    int     bandCount() const override;
    QString projection() const override;
    QString filePath() const override;
    bool    geoTransform(double gt[6]) const;
    void*   datasetHandle() const { return mDataset; }

private:
    void* mDataset = nullptr; // GDALDataset*
    QString mFilePath;
    int mWidth = 0, mHeight = 0, mBandCount = 0;
};

#endif // GDALSLCREADER_H
