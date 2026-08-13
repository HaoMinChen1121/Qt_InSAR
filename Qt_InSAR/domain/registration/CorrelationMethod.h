#ifndef REG_CORRELATIONMETHOD_H
#define REG_CORRELATIONMETHOD_H

#include <QString>

// 配准相关引擎 (可插拔) — 用户可对粗配准做覆盖, 精配准暂不开放
enum class CorrelationMethod {
    NCC,             // 空间域归一化互相关
    FFT_AMPLITUDE,   // FFT 幅度域互相关 + 亚像素峰值 (已实现)
    FFT_PHASE        // FFT 相位相关 (互功率谱归一化, 引擎未实现/实验性)
};

inline QString correlationMethodName(CorrelationMethod c) {
    switch (c) {
    case CorrelationMethod::NCC:           return QStringLiteral("NCC");
    case CorrelationMethod::FFT_AMPLITUDE: return QStringLiteral("FFT 幅度相关");
    case CorrelationMethod::FFT_PHASE:     return QStringLiteral("FFT 相位相关");
    }
    return QString();
}

#endif // REG_CORRELATIONMETHOD_H
