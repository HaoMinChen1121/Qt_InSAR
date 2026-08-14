#include "DemMapper.h"
#include "dataaccess/impl/GdalDemReader.h"
#include <algorithm>

LinearSlantRangeMapper::LinearSlantRangeMapper(const GeomTable& geom,
                                               GdalDemReader* dem,
                                               int imgW, int imgH)
    : mGeom(geom)
    , mDem(dem)
    , mImgW(imgW)
    , mImgH(imgH)
{
    // 合并产品的斜距范围 (第一/最后子条带的近距与远距)
    if (!geom.swaths.isEmpty()) {
        mRmin = geom.swaths.first().nearRange;
        const auto& last = geom.swaths.last();
        mRmax = last.nearRange + last.width * last.rangeSpacing;
    }
}

bool LinearSlantRangeMapper::sample(int row, int col, double* elevation)
{
    if (!elevation || !mDem || mImgW <= 0 || mImgH <= 0) return false;

    double R = 0, theta = 0;
    if (!mGeom.colGeometry(col, &R, &theta)) return false;

    // DEM 行: 方位比例线性
    int demRow = row * mDem->height() / mImgH;
    demRow = std::min(demRow, mDem->height() - 1);
    if (demRow != mCachedRow) {
        mDemRow = mDem->readElevationWindow(0, demRow, mDem->width(), 1);
        mCachedRow = demRow;
    }
    if (mDemRow.isEmpty()) return false;

    // DEM 列: 斜距比例线性
    int demCol = 0;
    if (mRmax > mRmin) {
        demCol = static_cast<int>((R - mRmin) / (mRmax - mRmin)
                                  * (mDem->width() - 1) + 0.5);
    } else {
        demCol = col * mDem->width() / mImgW;
    }
    demCol = std::clamp(demCol, 0, mDem->width() - 1);

    double h = static_cast<double>(mDemRow[demCol]);
    if (h < -1000.0) return false;
    *elevation = h;
    return true;
}
