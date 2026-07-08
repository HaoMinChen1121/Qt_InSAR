#ifndef ISLCWRITER_H
#define ISLCWRITER_H

#include <QString>
#include <complex>
#include <vector>

class ISlcWriter
{
public:
    virtual ~ISlcWriter() = default;

    virtual bool create(const QString& filePath,
        int width, int height, int bandCount,
        const QString& projection = QString()) = 0;
    virtual bool writeBand(int bandIndex,
        const QVector<std::complex<float>>& data) = 0;
    virtual void close() = 0;

    virtual QString lastError() const = 0;
};

#endif // ISLCWRITER_H
