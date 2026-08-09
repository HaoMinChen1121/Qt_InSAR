#ifndef GDALSLCWRITER_H
#define GDALSLCWRITER_H

#include "dataaccess/ISlcWriter.h"
#include <QVector>
#include <complex>

class GdalSlcWriter : public ISlcWriter
{
public:
    GdalSlcWriter();
    ~GdalSlcWriter() override;

    bool create(const QString& filePath,
        int width, int height, int bandCount,
        const QString& projection = QString()) override;
    bool writeBand(int bandIndex,
        const QVector<std::complex<float>>& data) override;
    bool writeRow(int row,
        const QVector<std::complex<float>>& data) override;
    void setGeoTransform(double x0, double dx, double rx,
                         double y0, double ry, double dy);
    void copyGeoreferencing(void* srcDataset, const QString& projection);
    bool flush();
    void setBlockRows(int n);
    void close() override;
    QString lastError() const override;

private:
    void* mDataset = nullptr;
    QString mLastError;

    int mBlockRows = 256;
    int mBufStartRow = 0;
    int mBufRows = 0;
    QVector<std::complex<float>> mRowBuf;
};

#endif // GDALSLCWRITER_H
