#ifndef IINTERFEROGRAMWRITER_H
#define IINTERFEROGRAMWRITER_H

#include <QString>
#include <QVector>
#include <complex>

class IInterferogramWriter
{
public:
    virtual ~IInterferogramWriter() = default;

    virtual bool create(const QString& filePath,
        int width, int height, bool isComplex) = 0;
    virtual bool writeComplex(const QVector<std::complex<float>>& data) = 0;
    virtual bool writePhase(const QVector<float>& data) = 0;
    virtual bool writeCoherence(const QVector<float>& data) = 0;
    virtual void close() = 0;
    virtual QString lastError() const = 0;
};

#endif // IINTERFEROGRAMWRITER_H
