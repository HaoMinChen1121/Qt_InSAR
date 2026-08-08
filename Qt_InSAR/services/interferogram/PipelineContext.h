#ifndef IFG_PIPELINECONTEXT_H
#define IFG_PIPELINECONTEXT_H

#include "domain/params/InterferogramParams.h"
#include "domain/QsarProduct.h"

struct IfgPipelineContext {
    const InterferogramParams* params = nullptr;

    // ── 输入 ──
    QString masterPath;
    QString slavePath;
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
