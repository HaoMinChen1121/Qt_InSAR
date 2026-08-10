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
    int       width;          // 列数
    int       height;         // 行数 (deburst 后)
};

// 将同一极化的 3 个 IW 相位/相干/复数文件合并为单幅影像
// iwFiles: 按 IW1→IW2→IW3 顺序的输入文件路径
// iwMetas: 对应的 range 几何参数
// outputBase: 输出路径前缀 (不含扩展名)
// 返回 true 表示成功
bool mergePhase(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath);

bool mergeCoherence(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath);

bool mergeComplex(
    const QVector<QString>& iwFiles,
    const QVector<IwMeta>&  iwMetas,
    const QString& outputPath);

} // namespace IWMerger

#endif // IWMERGER_H
