#include "GdalDemReader.h"

GdalDemReader::GdalDemReader() = default;
GdalDemReader::~GdalDemReader() { close(); }

bool GdalDemReader::open(const QString& /*filePath*/) { return false; }
void GdalDemReader::close() { mDataset = nullptr; }
bool GdalDemReader::isOpen() const { return mDataset != nullptr; }
QVector<float> GdalDemReader::readElevation() { return {}; }
QVector<float> GdalDemReader::readElevationWindow(
    int /*colStart*/, int /*rowStart*/, int /*colSize*/, int /*rowSize*/) { return {}; }
int     GdalDemReader::width() const { return mWidth; }
int     GdalDemReader::height() const { return mHeight; }
double* GdalDemReader::geoTransform() const { return const_cast<double*>(mGeoTransform); }
QString GdalDemReader::projection() const { return {}; }
double  GdalDemReader::noDataValue() const { return mNoData; }
