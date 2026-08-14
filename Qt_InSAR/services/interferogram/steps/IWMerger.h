#ifndef IWMERGER_H
#define IWMERGER_H

#include <QString>
#include <QVector>

// IW1/2/3 干涉图拼接 — 基于 range 几何合并为单幅宽幅产品
namespace IWMerger {

struct IwMeta {
    QString   swath;          // "IW1"/"IW2"/"IW3"
    double    nearRange;      // 斜距 (m)
    double    rangeSpacing;   // 距离向采样间距 (m)
    int       width;          // 列数 (多视后)
    int       height;         // 行数 (deburst 后, 多视后)
    int       fullResWidth = 0;  // 原分辨率列数 (几何重叠计算必须用全分辨率)
    int       rangeLooks  = 1;   // 距离向多视比 (trim 换算到输出列)
    int       azimuthOffset = 0; // 相对首个子条带的输出行偏移 (子条带间方位对齐)
};

// 将同一极化的 3 个 IW 相位/相干/复数文件合并为单幅影像
// iwFiles: 按 IW1→IW2→IW3 顺序的输入文件路径
// cohFiles: 对应相干性文件 (相位对齐估计权重; mergePhase/mergeComplex 用, 可为空)
// iwMetas: 对应的 range 几何 + 方位偏移参数
// 输出: 距离向重叠裁剪 + 方位向偏移对齐 + 裁剪到公共方位范围;
//       phase/complex 附带子条带相位一致性对齐 (常数+线性, 鲁棒估计)
// 返回 true 表示成功
bool mergePhase(
    const QVector<QString>& iwFiles,
    const QVector<QString>& cohFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath,
    bool alignEnabled = true);                  // phaseAlign=false 时跳过相位对齐

bool mergeCoherence(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath);

bool mergeComplex(
    const QVector<QString>& iwFiles,
    const QVector<QString>& cohFiles,
    const QVector<QString>& alignPhaseFiles,   // 对齐估计相位源 (用相位文件, 非复数文件)
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath,
    bool alignEnabled = true);

} // namespace IWMerger

#endif // IWMERGER_H
