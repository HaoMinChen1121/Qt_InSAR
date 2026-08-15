#ifndef GOLDSTEINFILTER_H
#define GOLDSTEINFILTER_H

#include <complex>
#include <vector>

namespace sar {

// Goldstein-Werner 自适应相位滤波 (复数进复数出, 相位连续性靠复数域保证)
// ifg: 输入复数干涉图 (w×h, 行主序); 幅度≈0 的像素视为无效, 原样保留
// alpha: 滤波强度 0~1 (0=仅相位谱, 1≈原图; 默认 0.5, ASF 用 0.6)
// patch: FFT 分块 (2 的幂, 默认 32), 50% 重叠三角加权重组
// smoothWin: 谱幅度平滑窗口 (奇数, 默认 3)
// tile: 大图分块处理尺寸 (默认 4096, 控制内存)
std::vector<std::complex<float>> goldsteinFilter(
    const std::complex<float>* ifg, int w, int h,
    double alpha, int patch, int smoothWin, int tile);

} // namespace sar

#endif // GOLDSTEINFILTER_H
