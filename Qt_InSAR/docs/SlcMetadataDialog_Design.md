# SLC Annotation 元数据检查对话框 — 设计文档 v1.0

## 1. 目标

在加载 SLC 产品后，提供一个可视化的 XML annotation 参数检查对话框，用于：
- 开发阶段验证 XML 流式解析是否完整正确
- 配准前快速比对主辅影像的处理参数一致性
- 异常数据排查（质量标志位、数据缺口）

---

## 2. 与现有 SarMetadataPanel 的关系

现有 `ui/SarMetadataPanel` 是一个 QFormLayout 的轻量 Dock 面板，显示约14个传感器摘要字段。
**新对话框不替代它**，而是互补：

| | SarMetadataPanel | SlcMetadataDialog（新） |
|---|---|---|
| 类型 | Dock Widget（常驻） | Dialog（按需弹出） |
| 数据来源 | `SarSensorInfo`（旧摘要结构） | `SlcAnnotation`（完整9大模块） |
| 字段数 | ~14 | ~100+，按模块树形组织 |
| 用途 | 快速查看传感器概览 | 深度检查 XML 解析完整性 |

---

## 3. 架构

```
用户操作（右键图层 / Ribbon按钮）
  │
  ▼
ApplicationController::showMetadataDialog(ISarProduct*)
  │
  ├─→ dynamic_cast<Sentinel1Product*>
  │     └─→ SlcAnnotation ann = s1->annotation()  // 新增接口
  │
  ▼
SlcMetadataDialog(SlcAnnotation, parent)
  │
  └─→ populateTree()  // 单次遍历，填充 QTreeWidget
```

### 3.1 ISarProduct 接口扩展

在 `ISarProduct` 中新增一个虚方法，返回完整的 annotation 数据：

```cpp
// ISarProduct.h
#include "dataaccess/annotation/SlcAnnotation.h"

class ISarProduct {
public:
    // ... 现有方法 ...
    virtual SlcAnnotation annotation() const { return SlcAnnotation(); }  // 默认空实现，S1 override
};
```

`Sentinel1Product` 在 `parseAnnotationStream()` 内部将 `reader.readAll()` 的结果缓存到 `mAnnotation` 成员，`annotation()` 返回它。

### 3.2 调用入口

右键菜单方式（推荐）：

```cpp
// 在 MainWindow::buildSlcLayerMenu() 中增加:
QAction* metaAction = menu->addAction("查看 SLC 元数据...");
connect(metaAction, &QAction::triggered, this, [this]() {
    emit slcMetadataRequested(/* 当前选中产品的 ISarProduct* */);
});
```

或者 Ribbon 按钮方式：

```cpp
// 在 createCategoryFile() 或 createCategoryRegistration() 中新增按钮
QAction* metaAction = createAction("XML 元数据", "...");
connect(metaAction, &QAction::triggered, ...);
```

---

## 4. UI 布局

```
┌──────────────────────────────────────────────────────────┐
│  SLC Annotation 元数据 — s1a-iw1-slc-vh-...001.xml       │
├────────────────────────────┬─────────────────────────────┤
│  QTreeWidget               │  QPlainTextEdit (read-only) │
│  ┌──────────────────┐      │  ┌───────────────────────┐  │
│  │ 树形导航          │      │  │ 选中字段的详细值       │  │
│  │                  │      │  │ 或数组/列表展开        │  │
│  │                  │      │  │                       │  │
│  │                  │      │  └───────────────────────┘  │
│  └──────────────────┘      │                             │
├────────────────────────────┴─────────────────────────────┤
│  [复制选中值]  [导出 JSON]  [复制全部]        [关闭]      │
└──────────────────────────────────────────────────────────┘
```

左右分栏：左侧 QTreeWidget（列：字段名 | 值），右侧为详细视图（数组、多项式展开）。

---

## 5. 树结构定义

9 个顶级节点，按 XML 文档顺序排列：

```
├─ 1. adsHeader (产品标识头)
│   ├─ missionId              "S1A"
│   ├─ productType            "SLC"
│   ├─ polarization           "VH"
│   ├─ mode                   "IW"
│   ├─ swath                  "IW1"
│   ├─ startTime              2026-06-05T10:35:10.066766
│   ├─ stopTime               2026-06-05T10:35:35.202108
│   ├─ absoluteOrbitNumber    64833
│   ├─ missionDataTakeId      "535382"
│   └─ passDirection          "Ascending"
│
├─ 2. qualityInformation (质量信息)
│   ├─ productQualityIndex    0.0          // 红色 if != 0
│   ├─ inputDataMeanOutsideNominalRange   false
│   ├─ inputDataStDevOutsideNominalRange  false
│   ├─ downlinkGapsSignificant            false  // 红色 if true
│   ├─ downlinkMissingSignificant         false  // 红色 if true
│   ├─ instrumentGapsSignificant          false  // 红色 if true
│   ├─ instrumentMissingSignificant       false  // 红色 if true
│   ├─ dopplerCentroidUncertain           false  // 红色 if true
│   ├─ iBiasSignificant                   false
│   ├─ qBiasSignificant                   false
│   ├─ iqGainSignificant                  false
│   └─ iqQuadratureSignificant            false
│
├─ 3. generalAnnotation (通用标注)
│   ├─ 3.1 productInformation
│   │   ├─ radarFrequency        5.405000e+09 Hz
│   │   ├─ rangeSamplingRate     6.434524e+07 Hz
│   │   └─ azimuthSteeringRate   1.590369 deg/s
│   ├─ 3.2 orbitList (16 vectors)
│   │   ├─ [0] 2026-06-05T10:34:06.607379
│   │   ├─ [1] 2026-06-05T10:34:16.607379
│   │   ├─ ...
│   │   └─ [15] 2026-06-05T10:36:36.700426
│   ├─ 3.3 attitudeList (25 records)
│   │   └─ [0] 2026-06-05T10:35:10.749997  roll=48.19°...
│   ├─ 3.4 azimuthFmRateList (11 groups)
│   │   └─ [0] t0=5.331e-03  poly=[-2334.7, 449683.0, -7.844e7]
│   │   ...
│   └─ 3.5 terrainHeightList (5 points)        // 当前未解析→显示"(未读取)"
│
├─ 4. imageAnnotation (影像参数)
│   ├─ 4.1 imageInformation
│   │   ├─ slantRangeTime          5.331287e-03 s
│   │   ├─ pixelValue              "Complex"
│   │   ├─ outputPixels            "16 bit Signed Integer"
│   │   ├─ azimuthTimeInterval     2.055556e-03 s
│   │   ├─ azimuthFrequency        4.864863e+02 Hz
│   │   ├─ rangePixelSpacing       2.329562 m
│   │   ├─ azimuthPixelSpacing     1.398743e+01 m
│   │   ├─ incidenceAngleMidSwath  33.95°
│   │   ├─ numberOfSamples         21512
│   │   ├─ numberOfLines           13446
│   │   └─ zeroDopMinusAcqTime     9.816883e+01 s
│   ├─ 4.2 processingInformation
│   │   ├─ rangeBandwidth          5.650000e+07 Hz
│   │   ├─ azimuthBandwidth        3.270000e+02 Hz
│   │   ├─ windowType              "Hamming"
│   │   ├─ windowCoefficient       0.75
│   │   ├─ numberOfLooks           1
│   │   ├─ orbitSource             "Auxiliary"
│   │   ├─ attitudeSource          "Downlink"
│   │   ├─ srgrApplied             false
│   │   └─ thermalNoiseCorrection  false
│   ├─ 4.3 ellipsoid
│   │   ├─ ellipsoidSemiMajor      6378137.0 m
│   │   └─ ellipsoidSemiMinor      6356752.3 m
│   └─ 4.4 imageStatistics
│       ├─ outputDataMean          (4.78e-03, -1.25e-03)
│       ├─ outputDataStdDev        (44.61, 44.60)
│       └─ outlierFlag             true   // 红色 if true
│
├─ 5. dopplerCentroid (多普勒质心)
│   ├─ estimateCount               11
│   ├─ [0] fineDceCount            20
│   └─ [1] ...
│
├─ 6. antennaPattern (天线方向图)
│   └─ (当前未存储)                             // 灰色斜体提示
│
├─ 7. swathTiming (TOPS Burst 时序)
│   ├─ linesPerBurst               1494
│   ├─ samplesPerBurst             21512
│   ├─ burstCount                  9
│   ├─ [0] azimuthTime             2026-06-05T10:35:10.066766
│   │   ├─ azimuthAnxTime          4.334566e+02 s
│   │   ├─ sensingTime             2026-06-05T10:35:11.195343
│   │   ├─ byteOffset              107883
│   │   ├─ burstIdRelative         21637
│   │   └─ burstIdAbsolute         139254617
│   └─ ...
│
├─ 8. geolocationGrid (地理定位网格)
│   ├─ pointCount                  210
│   ├─ [0] line=0 pixel=0  lat=26.89° lon=111.07°  h=254.99m
│   ├─ [1] line=0 pixel=1076  lat=26.90° lon=111.12°  h=209.99m
│   └─ ...
│
└─ 9. swathMerging / coordinateConversion
    └─ (SLC 不做子带合并和地距转换 — 正常)
```

### 5.1 颜色规则

| 颜色 | 条件 | 示例 |
|------|------|------|
| 红色 | 质量异常标志=true | `dopplerCentroidUncertain: true` |
| 橙色 | 值为 0/空/默认值（可能未读取） | `burstCount: 0` |
| 灰色 | 字段未存储/未解析 | `antennaPattern: (未读取)` |
| 正常 | 有有效值 | 其余 |

---

## 6. 弹出上下文

对话框不是只能看一个文件。一次加载主辅影像后，可以有三种方式触发：

1. **右键影像图层** → "查看 SLC 元数据..." → 弹出该影像的对话框
2. **主辅对比模式**：在配准页加一个"比对元数据"按钮 → 弹出对话框，左右并列两个树，差异字段高亮
3. **快捷键**：Ctrl+M 打开当前选中影像的元数据

---

## 7. 实现步骤

### Step 1：ISarProduct 增加 annotation() 接口

`ISarProduct.h` 增加 `#include "dataaccess/annotation/SlcAnnotation.h"` 和虚方法：

```cpp
virtual SlcAnnotation annotation() const { return SlcAnnotation(); }
```

`Sentinel1Product.h` 增加成员 `SlcAnnotation mAnnotation;`，重写 `annotation()`。

`Sentinel1Product.cpp` 在 `parseAnnotationFromReader()` 中将 `reader.readAll()` 的结果赋给 `mAnnotation`。

### Step 2：新建 SlcMetadataDialog

文件：
- `ui/SlcMetadataDialog.h`
- `ui/SlcMetadataDialog.cpp`

核心方法：

```cpp
class SlcMetadataDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SlcMetadataDialog(const SlcAnnotation& ann, QWidget* parent = nullptr);

private:
    void populateTree();
    void addNode(QTreeWidgetItem* parent, const QString& label, const QString& value,
                 const QColor& color = Qt::black);
    void addVectorNode(QTreeWidgetItem* parent, const QString& label,
                       const QVector<...>& vec, /* 子字段格式化函数 */);
    // 各模块填充子函数
    void fillAdsHeader(QTreeWidgetItem* root);
    void fillQuality(QTreeWidgetItem* root);
    void fillGeneralAnnotation(QTreeWidgetItem* root);
    void fillImageAnnotation(QTreeWidgetItem* root);
    void fillDoppler(QTreeWidgetItem* root);
    void fillSwathTiming(QTreeWidgetItem* root);
    void fillGeolocationGrid(QTreeWidgetItem* root);

    QTreeWidget* mTree;
    QPlainTextEdit* mDetailView;
    SlcAnnotation mAnn;
};
```

### Step 3：集成调用入口

在 `MainWindow` 或 `ApplicationController` 中响应菜单/按钮事件，获取 `ISarProduct*`，取出 `SlcAnnotation`，弹出 `SlcMetadataDialog`。

### Step 4：JSON 导出按钮

对话框底部按钮"导出 JSON"——将 `SlcAnnotation` 序列化为 JSON 文件，便于 diff 比对主辅影像参数。可复用 `QsarIO` 的风格，或直接用 `QJsonDocument` + 手写序列化。

---

## 8. 工作量估算

| 任务 | 估计行数 | 难度 |
|------|---------|------|
| ISarProduct 接口扩展 + Sentinel1Product 适配 | ~20 | 低 |
| SlcMetadataDialog 框架（布局、QTreeWidget、按钮） | ~120 | 低 |
| populateTree() + 9 个 fillXxx() 填充函数 | ~350 | 中（重复劳动） |
| 颜色规则逻辑 | ~40 | 低 |
| 右键菜单入口 | ~30 | 低 |
| JSON 导出 | ~60 | 低 |
| 主辅对比模式（Phase 2） | ~200 | 中 |
| **合计（Phase 1）** | **~620** | — |

---

## 9. 风险与注意事项

1. **内存**：`SlcAnnotation` 包含 geolocationGrid(210点)、dopplerEstimates(11组×20 fineDce=220点)、burstList(9个)等，单例约 50-80KB。缓存在 Sentinel1Product 中内存可控。

2. **树节点数量**：顶层9项，二级约100+项。QTreeWidget 完全够用，不需要 QTreeView+Model。210个 geolocationGrid 点全部展开会很多——默认折叠，点击时只显示前3个 + "..."，右侧 detail 面板显示完整列表。

3. **可扩展性**：当前只有 Sentinel-1，但 `SlcAnnotation` 结构已考虑到通用性。未来其他传感器（GF-3、ALOS-2）可以直接复用对话框，只需在 `ISarProduct::annotation()` 中填充对应字段。

4. **旧 `SarMetadataPanel`** 不需要修改，保持独立运作。
