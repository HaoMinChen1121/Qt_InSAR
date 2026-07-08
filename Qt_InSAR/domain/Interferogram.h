#ifndef INTERFEROGRAM_H
#define INTERFEROGRAM_H

#include <QString>
#include <QSize>

struct Interferogram
{
    QString   layerId;          // 图层ID
    QString   displayName;      // 显示名称
    QString   filePath;         // 文件路径
    QSize     rasterSize;       // 尺寸
    bool      isComplex = false;// 是否复数
    bool      flatEarthRemoved = false; // 已去平地效应
    bool      differential = false;     // 是否差分干涉
    double    minPhase = 0;     // 最小相位 (rad)
    double    maxPhase = 0;     // 最大相位 (rad)
    bool      visible = true;
};

#endif // INTERFEROGRAM_H
