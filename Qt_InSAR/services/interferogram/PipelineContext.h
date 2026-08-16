#ifndef IFG_PIPELINECONTEXT_H
#define IFG_PIPELINECONTEXT_H

#include "domain/params/InterferogramParams.h"
#include "domain/QsarProduct.h"
#include "domain/SarSensorInfo.h"
#include "domain/OrbitInfo.h"

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
    double  masterNearRange = 0.0;    // 距离相关 deramp 几何 (master 波段描述符)
    double  masterRangeSpacing = 0.0; // <=0 时回退常数 kt deramp
    // 解析差分多普勒旋转 (annotation dataDcPoly, 每 burst 一组系数 + t0)
    QVector<QVector<double>> masterDcPoly;
    QVector<double>          masterDcT0;
    QVector<QVector<double>> slaveDcPoly;
    QVector<double>          slaveDcT0;
    int     azimuthRampCorrectionSign = 0;  // 开发期诊断 (参数/环境变量已解析)
    int     outWidth  = 0;        // actual output dims (post-deburst)
    int     outHeight = 0;

    // ── S2 → S3: 去平地后路径 ──
    QString flatSourcePath;       // file to feed into differential stage

    // ── 阶段二 (逐极化, 合并产品): merge → flat → topo → visualization ──
    QString mergeOutputBase;      // ".../merge/S1_VV"
    QString geomTablePath;        // ".../merge/S1_VV_geom.json"
    QString flatOutputBase;       // ".../flat/S1_VV"
    QString diffOutputBase;       // ".../diff/S1_VV"
    QString visualizationOutputBase;  // ".../visualization/S1_VV"

    // ── 零多普勒定位 (TopoPhaseRemover) ──
    // master 轨道状态矢量 (主产品为原始 SLC 时可用; .qsar 主影像无轨道 → 地形去除失败)
    QList<OrbitStateVector> masterOrbit;
    QDateTime mergeTimeRef;         // 合并产品行0的方位时间参考 (IW1 首 burst 方位时间)
    double   masterPrf = 0.0;       // 方位采样频率 (Hz)
    int      mergeRow0Offset = 0;   // 合并裁剪量 (输出行): merge 行 r ↔ IW1 deburst 行 r+offset
    const QsarBand* masterBandInfo = nullptr;  // IW1 主波段 (burst 时间分段映射)

    // ── Goldstein 滤波输出 (coh 估计输入) ──
    QString filteredIfgPath;      // 滤波后复数干涉图 (diff 或 flat 滤波结果)

    // ── 输出 ──
    QsarBand outputBand;

    // ── 控制 ──
    bool    cancelled = false;
    QString errorMessage;
};

#endif // IFG_PIPELINECONTEXT_H
