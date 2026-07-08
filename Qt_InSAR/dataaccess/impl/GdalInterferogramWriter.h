#ifndef GDALINTERFEROGRAMWRITER_H
#define GDALINTERFEROGRAMWRITER_H

#include "dataaccess/IInterferogramWriter.h"

class GdalInterferogramWriter : public IInterferogramWriter
{
public:
    GdalInterferogramWriter();
    ~GdalInterferogramWriter() override;

    bool create(const QString& filePath,
        int width, int height, bool isComplex) override;
    bool writeComplex(const QVector<std::complex<float>>& data) override;
    bool writePhase(const QVector<float>& data) override;
    bool writeCoherence(const QVector<float>& data) override;
    void close() override;
    QString lastError() const override;

private:
    void* mDataset = nullptr;
    QString mLastError;
};

#endif // GDALINTERFEROGRAMWRITER_H
