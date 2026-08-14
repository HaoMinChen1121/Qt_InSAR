#ifndef IFG_DEMMAPPER_H
#define IFG_DEMMAPPER_H

#include <QString>
#include "services/interferogram/steps/GeomTable.h"

class GdalDemReader;

// ═══════════════════════════════════════════════════════════
//  DEM 高程采样抽象 (设计 §5.5)
//  v2 第一版: LinearSlantRangeMapper (斜距比例线性映射)
//  后续: GeometryBasedMapper (轨道+Doppler+地形几何) 无缝替换,
//        TopoPhaseRemover 零改动
// ═══════════════════════════════════════════════════════════
class IDemMapper {
public:
    virtual ~IDemMapper() = default;
    // 合并图像 (row, col) → DEM 高程 (m); false = 无有效高程
    virtual bool sample(int row, int col, double* elevation) = 0;
    virtual QString name() const = 0;
};

// 第一版: 列映射 = 斜距比例线性 (R(col) → DEM 列), 行映射 = 方位比例线性
// h < -1000 视为无效
class LinearSlantRangeMapper : public IDemMapper {
public:
    // dem: 非拥有指针 (生命周期由步骤保证, 整个 execute 期间有效)
    // imgW/imgH: 合并图像尺寸
    LinearSlantRangeMapper(const GeomTable& geom, GdalDemReader* dem,
                           int imgW, int imgH);

    bool sample(int row, int col, double* elevation) override;
    QString name() const override { return QStringLiteral("LinearSlantRange"); }

private:
    GeomTable mGeom;
    GdalDemReader* mDem = nullptr;
    int mImgW = 0, mImgH = 0;
    double mRmin = 0, mRmax = 0;
    int mCachedRow = -1;
    QVector<float> mDemRow;
};

#endif // IFG_DEMMAPPER_H
