#include "GdalSlcReader.h"

GdalSlcReader::GdalSlcReader() = default;
GdalSlcReader::~GdalSlcReader() { close(); }

bool GdalSlcReader::open(const QString& /*filePath*/)
{
    // TODO: 通过GDAL打开SLC文件
    return false;
}

void GdalSlcReader::close()
{
    // TODO: 关闭GDAL数据集
    mDataset = nullptr;
}

bool GdalSlcReader::isOpen() const { return mDataset != nullptr; }

QVector<std::complex<float>> GdalSlcReader::readBand(int /*bandIndex*/)
{
    // TODO: 读取全波段复数数据
    return {};
}

QVector<std::complex<float>> GdalSlcReader::readBandWindow(int /*bandIndex*/,
        int /*colStart*/, int /*rowStart*/, int /*colSize*/, int /*rowSize*/)
    {
    // TODO: 窗口读取复数数据
    return {};
}

int     GdalSlcReader::width() const { return mWidth; }
int     GdalSlcReader::height() const { return mHeight; }
int     GdalSlcReader::bandCount() const { return mBandCount; }
QString GdalSlcReader::projection() const { return {}; }
QString GdalSlcReader::filePath() const { return mFilePath; }
