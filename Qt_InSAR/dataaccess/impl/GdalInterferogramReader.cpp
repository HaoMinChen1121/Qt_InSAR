#include "GdalInterferogramReader.h"

GdalInterferogramReader::GdalInterferogramReader() = default;
GdalInterferogramReader::~GdalInterferogramReader() { close(); }

bool GdalInterferogramReader::open(const QString& /*filePath*/)
{
    // TODO: GDAL打开干涉图文件
    return false;
}
void GdalInterferogramReader::close() { mDataset = nullptr; }
bool GdalInterferogramReader::isOpen() const { return mDataset != nullptr; }
QVector<std::complex<float>> GdalInterferogramReader::readComplex() { return {}; }
QVector<float> GdalInterferogramReader::readPhase() { return {}; }
QVector<float> GdalInterferogramReader::readCoherence() { return {}; }
int     GdalInterferogramReader::width() const { return mWidth; }
int     GdalInterferogramReader::height() const { return mHeight; }
QString GdalInterferogramReader::filePath() const { return mFilePath; }
