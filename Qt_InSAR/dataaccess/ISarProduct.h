#ifndef ISARPRODUCT_H
#define ISARPRODUCT_H

#include "domain/SarSensorInfo.h"
#include "domain/SarProductType.h"
#include "domain/OrbitInfo.h"
#include <QString>
#include <QList>
#include <QSize>
#include <QDateTime>
#include <complex>

// 波段描述符（SAR 版本，增加极化、分辨率）
struct SarBandDescriptor {
    int     index = -1;           // 波段序号 (0-based)
    QString polarization;         // "VV", "VH", "HH", "HV"
    QString subSwath;             // "IW1", "IW2", "IW3" (Sentinel-1 TOPS)
    QString rasterPath;           // GDAL 可打开的数据文件路径
    double  resolution  = 0.0;    // 空间分辨率 (m)
    QSize   rasterSize;           // 像素尺寸 (cols × rows)
    QString dataType;             // GDAL 数据类型名称
    bool    isComplex = false;    // 复数数据

    // TOPSAR burst 信息 (Sentinel-1 IW)
    int     linesPerBurst = 0;    // 每个 burst 方位向行数
    int     burstCount = 0;       // burst 数 (通常 9)
    QVector<int> burstStartLines; // 每个 burst 在整体影像中的起行号
    QVector<QDateTime> burstAzimuthTimes; // 每个 burst 的方位向零多普勒时间
    double  azimuthFmRate = 0.0;          // 方位向调频率 (Hz/s, 用于TOPS deramping)
    double  azimuthSteeringRate = 0.0;     // 天线转向速率 (deg/s, TOPS deburst)
    double  azimuthFrequency = 0.0;        // 有效方位向PRF (Hz, 每子条带不同)
};

class ISarProduct {
public:
    virtual ~ISarProduct() = default;

    // 打开/关闭产品
    virtual bool open(const QString& path) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 产品类型
    virtual SarProductType productType() const = 0;
    virtual QString sensorType() const = 0;
    virtual QString productId() const = 0;
    virtual QString acquisitionMode() const = 0;

    // 波段列表
    virtual QList<SarBandDescriptor> bands() const = 0;
    virtual QList<SarBandDescriptor> bandsByPolarization(const QString& pol) const = 0;
    virtual int bandCount() const { return bands().size(); }

    // 元数据
    virtual SarSensorInfo sensorInfo() const = 0;
    virtual QList<OrbitStateVector> orbitStateVectors() const = 0;
    virtual DopplerInfo dopplerCentroid() const = 0;

    // 预览图
    virtual QString previewImagePath() const = 0;
    virtual QString originalPath() const = 0;

    // 读取复数数据 (SLC)
    virtual QVector<std::complex<float>> readComplexSamples(
        int bandIndex, int x0, int y0, int w, int h) = 0;
};

#endif // ISARPRODUCT_H
