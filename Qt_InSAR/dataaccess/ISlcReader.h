#ifndef ISLCREADER_H
#define ISLCREADER_H

#include <QString>
#include <QVector>
#include <complex>

class ISlcReader
{
public:
    virtual ~ISlcReader() = default;

    virtual bool open(const QString& filePath) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 读取波段数据 (复数)
    virtual QVector<std::complex<float>> readBand(int bandIndex = 0) = 0;
    // 窗口读取
    virtual QVector<std::complex<float>> readBandWindow(int bandIndex,
        int colStart, int rowStart, int colSize, int rowSize) = 0;

    // 元信息
    virtual int     width() const = 0;
    virtual int     height() const = 0;
    virtual int     bandCount() const = 0;
    virtual QString projection() const = 0;
    virtual QString filePath() const = 0;
};

#endif // ISLCREADER_H
