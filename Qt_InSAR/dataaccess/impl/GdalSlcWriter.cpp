#include "GdalSlcWriter.h"

GdalSlcWriter::GdalSlcWriter() = default;
GdalSlcWriter::~GdalSlcWriter() { close(); }

bool GdalSlcWriter::create(const QString& /*filePath*/,
    int /*width*/, int /*height*/, int /*bandCount*/,
        const QString& /*projection*/)
    {
    // TODO: 创建GDAL SLC文件
    return false;
}

bool GdalSlcWriter::writeBand(int /*bandIndex*/,
        const QVector<std::complex<float>>& /*data*/)
    {
    // TODO: 写入复数波段
    return false;
}

void GdalSlcWriter::close()
{
    mDataset = nullptr;
}

QString GdalSlcWriter::lastError() const { return mLastError; }
