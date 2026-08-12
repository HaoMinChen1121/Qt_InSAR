#ifndef SLCANNOTATIONREADER_H
#define SLCANNOTATIONREADER_H

#include "SlcAnnotation.h"
#include <QXmlStreamReader>
#include <QString>
#include <QIODevice>

// ═══════════════════════════════════════════════════════════
//  QXmlStreamReader 流式 S1 annotation XML 解析器
//  支持三层按需解析 (快速扫描 → 核心加载 → 辅助加载)
//  支持 VSI 路径和本地文件
// ═══════════════════════════════════════════════════════════

class QFile;

class SlcAnnotationReader {
public:
    SlcAnnotationReader() = default;
    ~SlcAnnotationReader();

    // 打开 XML 文件 (支持本地路径和 /vsizip/ 等 GDAL VSI 路径)
    bool open(const QString& filePath);
    // 直接从内存数据打开 (VSI 路径优化，避免临时文件)
    bool openFromData(const QByteArray& data);
    void close();
    bool isOpen() const { return mDevice != nullptr; }

    // ═══════════════════════════════════════════
    //  第1层：快速扫描 (像对选择阶段)
    // ═══════════════════════════════════════════
    ProductIdentity readIdentity();
    QString readPassDirection();

    // ═══════════════════════════════════════════
    //  第2层：核心加载 (配准/干涉阶段)
    // ═══════════════════════════════════════════
    QVector<OrbitVector> readOrbitVectors();
    double readRadarFrequency();
    double readRangeSamplingRate();
    double readAzimuthSteeringRate();
    double readSlantRangeTime();
    double readIncidenceAngleMidSwath();
    double readAzimuthFrequency();
    double readAzimuthPixelSpacing();
    double readZeroDopMinusAcqTime();
    int    readNumberOfSamples();
    int    readNumberOfLines();
    int    readLinesPerBurst();
    int    readSamplesPerBurst();

    QVector<BurstData> readBurstTiming();
    QVector<GeolocationPoint> readGeolocationGrid();
    QVector<DopplerEstimate> readDopplerCentroid();
    QVector<AzimuthFmRate> readAzimuthFmRates();

    // ═══════════════════════════════════════════
    //  第3层：辅助加载 (辐射校正/质量检查)
    // ═══════════════════════════════════════════
    ProcessingConsistency readProcessingConsistency();
    void readEllipsoidParams(double& semiMajor, double& semiMinor);
    QVector<AttitudeData> readAttitudeList();
    QualityInfo readQualityInfo();
    ImageStatistics readImageStatistics();
    QString readPixelValue();
    QString readOutputPixels();
    double readAzimuthTimeInterval();

    // ═══════════════════════════════════════════
    //  一次性全读 (单遍扫描，仅一次 reset)
    // ═══════════════════════════════════════════
    SlcAnnotation readAll();

private:
    // 从当前位置流式扫描到指定起始元素
    bool seekToElement(const QString& elementName);
    // 读取当前元素的文本内容
    QString readElementText();
    // 解析空格分隔多项式 → QVector<double>
    static QVector<double> parsePolynomial(const QString& text);
    // ISO8601 微秒解析 (Qt5 兼容)
    static QDateTime parseIso8601(const QString& s);
    // 跳过当前子树 (含所有子元素)
    void skipSubtree();
    // 重置流到文件开头
    void reset();

    // 内部版本: 从当前位置继续扫描 (不reset), 供 readAll() 单遍扫描使用
    QVector<OrbitVector>       readOrbitVectorsNoReset();
    QVector<AttitudeData>      readAttitudeListNoReset();
    QVector<AzimuthFmRate>     readAzimuthFmRatesNoReset();
    QVector<DopplerEstimate>   readDopplerCentroidNoReset();
    QVector<GeolocationPoint>  readGeolocationGridNoReset();
    QVector<BurstData>         readBurstTimingNoReset();

    QXmlStreamReader* mXml    = nullptr;
    QIODevice*        mDevice = nullptr;
    QFile*            mFile   = nullptr;
    QByteArray        mData;  // 持有内存数据 (openFromData 路径)
    QString           mFilePath;
};

#endif // SLCANNOTATIONREADER_H
