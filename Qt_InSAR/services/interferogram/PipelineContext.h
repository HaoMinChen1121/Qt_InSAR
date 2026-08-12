#ifndef IFG_PIPELINECONTEXT_H
#define IFG_PIPELINECONTEXT_H

#include "domain/params/InterferogramParams.h"
#include "domain/QsarProduct.h"
#include "domain/SarSensorInfo.h"

struct IfgPipelineContext {
    const InterferogramParams* params = nullptr;
    SarSensorInfo              masterSensorInfo;  // 从主产品XML解析的传感器元数据

    // ── 输入 ──
    QString masterPath;         // master VSI 路径 (或本地路径)
    QString slavePath;
    QString masterZip;          // master ZIP 文件路径 (用于 SDR)
    QString masterEntry;        // master ZIP 内 TIFF entry (用于 SDR)
    int     width  = 0;
    int     height = 0;
    const QsarBand* burstInfo = nullptr;  // TOPSAR burst metadata (nullable)

    // ── S1 → S2: 干涉图输出 ──
    QString ifgOutputBase;        // e.g. ".../ifg/IW1_VV"
    int     outWidth  = 0;        // actual output dims (post-deburst)
    int     outHeight = 0;

    // ── S2 → S3: 去平地后路径 ──
    QString flatSourcePath;       // file to feed into differential stage

    // ── 输出 ──
    QsarBand outputBand;

    // ── 控制 ──
    bool    cancelled = false;
    QString errorMessage;
};

#endif // IFG_PIPELINECONTEXT_H
