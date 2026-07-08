#ifndef IDEMREADER_H
#define IDEMREADER_H

#include <QString>
#include <QVector>

class IDemReader
{
public:
    virtual ~IDemReader() = default;

    virtual bool open(const QString& filePath) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    virtual QVector<float> readElevation() = 0;
    // 窗口读取
    virtual QVector<float> readElevationWindow(
        int colStart, int rowStart, int colSize, int rowSize) = 0;

    virtual int     width() const = 0;
    virtual int     height() const = 0;
    virtual double* geoTransform() const = 0;  // 6元素仿射变换
    virtual QString projection() const = 0;
    virtual double  noDataValue() const = 0;
};

#endif // IDEMREADER_H
