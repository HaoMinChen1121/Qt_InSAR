#ifndef SARCOMPLEXTYPES_H
#define SARCOMPLEXTYPES_H

#include <complex>

// GDAL GDT_CFloat32 兼容的显式复数类型
// std::complex<float> 的 {re, im} 布局是编译器ABI保证而非C++标准保证.
// 在 RasterIO 边界使用此结构体, 消除隐式依赖.
struct CFloat32 {
    float re;
    float im;

    CFloat32() = default;
    CFloat32(float r, float i) : re(r), im(i) {}
    explicit CFloat32(const std::complex<float>& c) : re(c.real()), im(c.imag()) {}

    operator std::complex<float>() const { return {re, im}; }
};

static_assert(sizeof(CFloat32) == sizeof(std::complex<float>),
    "CFloat32 must match std::complex<float> layout");
static_assert(sizeof(CFloat32) == 2 * sizeof(float),
    "CFloat32 must be exactly 2 floats");
static_assert(alignof(CFloat32) == alignof(std::complex<float>),
    "CFloat32 must have same alignment as std::complex<float>");

#endif // SARCOMPLEXTYPES_H
