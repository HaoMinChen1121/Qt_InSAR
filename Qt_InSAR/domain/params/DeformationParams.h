#ifndef DEFORMATIONPARAMS_H
#define DEFORMATIONPARAMS_H

#include <QString>

struct DeformationParams {
    // 相位转换
    QString conversionMode;      // "Phase2Deformation" / "Phase2Height"
    QString projectionDirection; // "LOS" / "Vertical" / "Horizontal"

    // 几何参数
    double wavelength     = 0.03125;  // 波长 (m)
    double incidenceAngle = 35.0;     // 入射角 (deg)
    double slantRange     = 850000.0; // 斜距 (m)
    double baselinePerp   = 150.0;    // 垂直基线 (m)

    // 时序方法
    QString tsMethod;            // "Stacking" / "SBAS" / "PS-InSAR"
    int     minTempBaseline  = 30;       // 最小时间基线 (天)
    double  maxSpatialBaseline = 300.0;  // 最大空间基线 (m)
    QString refPointPath;                // 参考点文件

    // 解缠
    QString unwrapMethod;        // "2D" / "3D" / "SNAPHU"

    // 大气校正
    QString atmMethod;           // "GACOS" / "Linear" / "None"
    QString gacosZtdPath;
    QString gacosStdPath;
    bool    linearRamp    = true;
    bool    elevationCorr = true;

    // 输出
    QString outputDir;
    QString outputPrefix;
    QString outputFormat;        // "GeoTIFF" / "ENVI" / "NetCDF"
    bool    exportRateMap = true;
    bool    exportTimeSeries = true;
    bool    exportKml      = false;
};

#endif // DEFORMATIONPARAMS_H
