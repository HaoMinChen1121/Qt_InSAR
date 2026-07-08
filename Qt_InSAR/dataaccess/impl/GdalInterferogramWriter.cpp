#include "GdalInterferogramWriter.h"

GdalInterferogramWriter::GdalInterferogramWriter() = default;
GdalInterferogramWriter::~GdalInterferogramWriter() { close(); }

bool GdalInterferogramWriter::create(const QString& /*filePath*/,
    int /*width*/, int /*height*/, bool /*isComplex*/) { return false; }
bool GdalInterferogramWriter::writeComplex(const QVector<std::complex<float>>& /*data*/) { return false; }
bool GdalInterferogramWriter::writePhase(const QVector<float>& /*data*/) { return false; }
bool GdalInterferogramWriter::writeCoherence(const QVector<float>& /*data*/) { return false; }
void GdalInterferogramWriter::close() { mDataset = nullptr; }
QString GdalInterferogramWriter::lastError() const { return mLastError; }
