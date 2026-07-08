#ifndef GDALINTERFEROGRAMREADER_H
#define GDALINTERFEROGRAMREADER_H

#include "dataaccess/IInterferogramReader.h"

class GdalInterferogramReader : public IInterferogramReader
{
public:
    GdalInterferogramReader();
    ~GdalInterferogramReader() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;

    QVector<std::complex<float>> readComplex() override;
    QVector<float> readPhase() override;
    QVector<float> readCoherence() override;

    int     width() const override;
    int     height() const override;
    QString filePath() const override;

private:
    void* mDataset = nullptr;
    QString mFilePath;
    int mWidth = 0, mHeight = 0;
};

#endif // GDALINTERFEROGRAMREADER_H
