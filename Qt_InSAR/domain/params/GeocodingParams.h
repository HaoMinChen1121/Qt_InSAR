#ifndef GEOCODINGPARAMS_H
#define GEOCODINGPARAMS_H

#include <QString>

struct GeocodingParams
{
    // 地理编码
    QString   method = "RangeDoppler";      // "RangeDoppler" / "Polynomial"
    int       polynomialOrder = 3;           // 多项式阶数
    QString   demPath;                       // DEM(用于地形校正)
    bool      terrainCorrection = true;

    // 坐标转换
    int       targetEpsg = 4326;             // 目标EPSG代码
    double    outputResolution = 0;          // 输出分辨率 (0=自动)
    double    outputWest = 0, outputEast = 0;// 输出范围
    double    outputSouth = 0, outputNorth = 0;

    // 输出
    QString   outputFormat = "GeoTIFF";      // "GeoTIFF" / "ENVI"
    QString   outputDir;
    QString   outputPrefix = "geocoded";
};

#endif // GEOCODINGPARAMS_H
