#ifndef SENTINEL1PRODUCT_H
#define SENTINEL1PRODUCT_H

#include "dataaccess/ISarProduct.h"
#include <QStringList>
#include <QDomDocument>
#include <QDateTime>
#include <QMap>

class Sentinel1Product : public ISarProduct {
public:
    Sentinel1Product();
    ~Sentinel1Product() override;

    bool open(const QString& path) override;
    void close() override;
    bool isOpen() const override { return mIsOpen; }

    SarProductType productType() const override  { return mProductType; }
    QString sensorType() const override          { return mSensorType; }
    QString productId() const override           { return mProductId; }
    QString acquisitionMode() const override     { return mAcquisitionMode; }

    QList<SarBandDescriptor> bands() const override              { return mBands; }
    QList<SarBandDescriptor> bandsByPolarization(const QString& pol) const override;
    int bandCount() const override                               { return mBands.size(); }

    SarSensorInfo sensorInfo() const override                    { return mSensorInfo; }
    QList<OrbitStateVector> orbitStateVectors() const override   { return mOrbitVectors; }
    DopplerInfo dopplerCentroid() const override                 { return mDoppler; }

    QString previewImagePath() const override  { return mPreviewPath; }
    QString originalPath() const override      { return mOriginalPath; }

    QVector<std::complex<float>> readComplexSamples(
        int bandIndex, int x0, int y0, int w, int h) override;

private:
    bool openDirectory(const QString& safDir);
    bool openZip(const QString& zipPath);
    bool parseManifest(const QString& manifestPath);
    bool parseAnnotation(const QString& annotationPath);
    void discoverMeasurementFiles(const QString& measurementDir);

    // 从 Sentinel-1 文件名推断元数据
    struct S1FileNameInfo {
        QString missionId;       // S1A / S1B
        QString mode;            // IW / EW / SM / WV
        QString productType;     // SLC / GRD
        QString resolution;      // F / H / M
        QString polarization;    // SH/SV/DH/DV 或 vv/vh/hh/hv
        QString subSwath;        // IW1/IW2/IW3 或 iw1/iw2/iw3
    };
    static S1FileNameInfo parseS1FileName(const QString& fileName);

    QString xmlFirstElementText(const QDomElement& parent, const QString& tagName) const;
    double xmlFirstElementDouble(const QDomElement& parent, const QString& tagName) const;

    bool mIsOpen = false;

    // 产品标识
    SarProductType mProductType = SarProductType::Unknown;
    QString mSensorType;
    QString mProductId;
    QString mAcquisitionMode;

    // 产品路径
    QString mOriginalPath;
    QString mPreviewPath;

    // 元数据
    QList<SarBandDescriptor> mBands;
    SarSensorInfo mSensorInfo;
    QList<OrbitStateVector> mOrbitVectors;
    DopplerInfo mDoppler;

    // 解析状态 (跨函数共享)
    QString mMissionId;
    QDateTime mAcquisitionStart;
    QStringList mPolarizations;
    int mOrbitNumberAbs = 0;
    int mOrbitNumberRel = 0;

    // Burst 解析暂存 (每次 parseAnnotation 更新，供 discoverMeasurementFiles 使用)
    int mParsedLinesPerBurst = 0;
    int mParsedRangeSamples = 0;
    QVector<int> mParsedBurstStarts;
    QVector<QDateTime> mParsedBurstAzimuthTimes;
    double mParsedAzimuthFmRate = 0.0;
    double mParsedAzimuthSteeringRate = 0.0;
    QMap<QString, double> mParsedAzimuthFreqBySwath;      // 每子条带的有效方位向PRF
    QMap<QString, int>    mParsedLinesPerBurstBySwath;    // 每子条带的burst行数
    QMap<QString, QVector<int>>      mParsedBurstStartsBySwath;   // 每子条带的burst起始行
    QMap<QString, QVector<QDateTime>> mParsedBurstTimesBySwath;   // 每子条带的burst azimuth时间
};

#endif // SENTINEL1PRODUCT_H
