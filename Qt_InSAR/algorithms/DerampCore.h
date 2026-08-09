#ifndef DERAMPCORE_H
#define DERAMPCORE_H

#include "ComplexSoA.h"
#include "SimdMath.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace sar {

// ═══════════════════════════════════════════════════
//  Burst 级别一次性 deramp (SoA + SIMD)
//  消除原per-strip的33x冗余计算
// ═══════════════════════════════════════════════════
inline void applyDeramp_SoA(ComplexSoA& data, int sW, int L,
                             int burstRow0, int burstIdx,
                             double prf, double kt)
{
    for (int r = 0; r < L; ++r) {
        int slaveRow = burstRow0 + r;
        int sbIdx = slaveRow / L;
        if (sbIdx < 0) sbIdx = 0;
        if (sbIdx > burstIdx + 1) sbIdx = burstIdx + 1;
        double eta_S = (slaveRow - sbIdx * L - L / 2.0) / prf;
        double dp = -M_PI * kt * eta_S * eta_S;
        float dCos = static_cast<float>(std::cos(dp));
        float dSin = static_cast<float>(std::sin(dp));

        int rowOff = r * sW;
        cplxRotate(data.re + rowOff, data.im + rowOff,
                   data.re + rowOff, data.im + rowOff,
                   sW, dCos, dSin);
    }
}

} // namespace sar

#endif // DERAMPCORE_H
