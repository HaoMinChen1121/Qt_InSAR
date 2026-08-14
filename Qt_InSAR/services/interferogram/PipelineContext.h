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
    const QsarBand* burstInfo = nullptr;       // slave burst 元数据 (nullable)
    const QsarBand* masterBurstInfo = nullptr; // master 逐 burst 方位时间 (deburst t_ref 来源)

    // ── S1 → S2: deburst 输出与临时块交换 ──
    QString deburstOutputBase;    // e.g. ".../deburst/IW1_VV"
    QString burstBlockBase;       // 临时 burst 块路径基 ".../deburst/.tmp/IW1_VV"
    double  azimuthFmRate = 0.0;  // master k_t (Hz/s), 由服务从产品/轨道填充
    int     azimuthRampCorrectionSign = 0;  // 开发期诊断 (参数/环境变量已解析)
    int     outWidth  = 0;        // actual output dims (post-deburst)
    int     outHeight = 0;

    // ── S2 → S3: 去平地后路径 ──
    QString flatSourcePath;       // file to feed into differential stage

    // ── 阶段二 (逐极化, 合并产品): merge → flat → topo ──
    QString mergeOutputBase;      // ".../merge/S1_VV"
    QString geomTablePath;        // ".../merge/S1_VV_geom.json"
    QString flatOutputBase;       // ".../flat/S1_VV"
    QString diffOutputBase;       // ".../diff/S1_VV"

    // ── 输出 ──
    QsarBand outputBand;

    // ── 控制 ──
    bool    cancelled = false;
    QString errorMessage;
};

#endif // IFG_PIPELINECONTEXT_H
