#ifndef GDALSLCWRITER_H
#define GDALSLCWRITER_H

#include "dataaccess/ISlcWriter.h"

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
    void close() override;
    QString lastError() const override;

private:
    void* mDataset = nullptr;
    QString mLastError;
};

#endif // GDALSLCWRITER_H
