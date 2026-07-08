#ifndef IINTERFEROGRAMREADER_H
#define IINTERFEROGRAMREADER_H

#include <QString>
#include <QVector>
#include <complex>

class IInterferogramReader
{
public:
    virtual ~IInterferogramReader() = default;

    virtual bool open(const QString& filePath) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual QVector<std::complex<float>> readComplex() = 0;        // 复数干涉图
    virtual QVector<float> readPhase() = 0;                         // 相位 (rad)
    virtual QVector<float> readCoherence() = 0;                     // 相干性

    virtual int     width() const = 0;
    virtual int     height() const = 0;
    virtual QString filePath() const = 0;
};

#endif // IINTERFEROGRAMREADER_H
