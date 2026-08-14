#ifndef IFG_GEOMTABLE_H
#define IFG_GEOMTABLE_H

#include <QString>
#include <QVector>

// ═══════════════════════════════════════════════════════════
//  合并产品逐列几何表 (IWMerge 后由服务写入 merge/S1_VV_geom.json,
//  FlatEarthRemover/TopoPhaseRemover 逐列查表)
//  设计 §5.3③: 逐列 R(col) = nearRange + (col-startCol)·rangeSpacing;
//  入射角由几何推导 (不依赖 annotation 逐子条带入射角):
//    cosθ = (R² + 2·H·Re + H²) / (2·R·(H+Re))
// ═══════════════════════════════════════════════════════════

struct SwathGeom {
    QString name;
    int    startCol = 0;        // 合并图像中的起始列
    int    width = 0;           // 合并图像中的列数 (已扣除重叠裁剪)
    double nearRange = 0.0;     // 该子条带近距 (m)
    double rangeSpacing = 0.0;  // 输出列间距 (m/列, = 全分辨率间距 × rgLooks)
};

struct GeomTable {
    int width = 0;              // 合并图像总列数
    QVector<SwathGeom> swaths;

    bool load(const QString& path);
    bool save(const QString& path) const;

    // 逐列: 斜距 R(m) + 入射角 θ(rad); false = 列越界或无几何
    bool colGeometry(int col, double* range, double* incidenceRad) const;

    // 平台高度 (m) — S1 近圆轨道近似 693km; 几何推导用
    static constexpr double kPlatformHeight = 693000.0;
    static constexpr double kEarthRadius = 6378137.0;
};

#endif // IFG_GEOMTABLE_H
