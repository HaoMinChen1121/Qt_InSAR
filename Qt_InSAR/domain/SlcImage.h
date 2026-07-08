#ifndef SLCIMAGE_H
#define SLCIMAGE_H

#include <QString>
#include <QSize>

struct SlcImage
{
    QString   layerId;         // 图层唯一ID
    QString   displayName;     // 显示名称
    QString   filePath;        // 文件路径
    QSize     rasterSize;      // 距离向×方位向
    int       bandCount = 1;   // 波段数 (通常单波段)
    QString   polarization;    // 极化方式 (VV/VH/HH/HV)
    double    wavelength = 0;  // 波长 (m)
    double    rangeSpacing = 0;// 距离向采样间隔 (m)
    double    azimuthSpacing = 0;// 方位向采样间隔 (m)
    double    nearRange = 0;   // 近距 (m)
    double    farRange = 0;    // 远距 (m)
    double    prf = 0;         // 脉冲重复频率 (Hz)
    double    centerFreq = 0;  // 中心频率 (Hz)
    bool      visible = true;
};

#endif // SLCIMAGE_H
