#ifndef REG_PROCESSINGLEVEL_H
#define REG_PROCESSINGLEVEL_H

#include <QString>

// 处理等级 — 只调整参数预设 (窗口/点数/阶数), 不改变物理模型
enum class ProcessingLevel { Fast, Standard, High };

inline QString processingLevelName(ProcessingLevel l) {
    switch (l) {
    case ProcessingLevel::Fast:     return QStringLiteral("快速");
    case ProcessingLevel::Standard: return QStringLiteral("标准");
    case ProcessingLevel::High:     return QStringLiteral("高精度");
    }
    return QString();
}

#endif // REG_PROCESSINGLEVEL_H
