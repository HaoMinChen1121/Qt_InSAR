#include "OrbitDataReader.h"

OrbitDataReader::OrbitDataReader() = default;
OrbitDataReader::~OrbitDataReader() { close(); }

bool OrbitDataReader::open(const QString& /*filePath*/)
{
    // TODO: 解析轨道数据文件（如XML/EOF格式的精密轨道）
    return false;
}
void OrbitDataReader::close() { mOpen = false; }
bool OrbitDataReader::isOpen() const { return mOpen; }

OrbitInfo OrbitDataReader::readOrbit()
{
    return mOrbit;
}

QVector<OrbitStateVector> OrbitDataReader::interpolate(
        double /*startTime*/, double /*endTime*/, double /*interval*/)
    {
    // TODO: Lagrange/样条插值生成均匀时间采样的轨道状态向量
    return {};
}
