#ifndef REG_DATAREADER_H
#define REG_DATAREADER_H

#include "IRegStep.h"

// Step 1: SLC数据读取 — GDAL读取TIFF复数数据, 填充SlcDataBundle
class DataReader : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("1. Data Reader"); }
};

#endif
