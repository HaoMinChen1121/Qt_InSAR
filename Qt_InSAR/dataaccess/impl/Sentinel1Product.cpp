#include "Sentinel1Product.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDomDocument>
#include <QDebug>
#include <algorithm>
#include <cmath>

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_port.h>
#include <cpl_vsi.h>
#include <cpl_string.h>
#include "dataaccess/annotation/SlcAnnotationReader.h"
#include "domain/SarComplexTypes.h"

static constexpr int S1_ORBIT_REPEAT_CYCLE = 175; // Sentinel-1 12天/175轨道重复周期

// 前向声明 (定义在 parseAnnotationStream 之前)
class SlcAnnotationReader;
static bool parseAnnotationFromReader(SlcAnnotationReader& reader,
    SarSensorInfo& mSensorInfo, DopplerInfo& mDoppler, QList<OrbitStateVector>& mOrbitVectors,
    int& mOrbitNumberAbs, int& mOrbitNumberRel,
    int& mParsedLinesPerBurst, int& mParsedSamplesPerBurst, int& mParsedRangeSamples,
    QMap<QString, double>& mParsedAzimuthFmRateBySwath,
    QMap<QString, double>& mParsedAzimuthSteeringRateBySwath,
    QMap<QString, double>& mParsedAzimuthFreqBySwath,
    QMap<QString, int>& mParsedLinesPerBurstBySwath,
    QMap<QString, int>& mParsedSamplesPerBurstBySwath,
    QMap<QString, QVector<int>>& mParsedBurstStartsBySwath,
    QMap<QString, QVector<QDateTime>>& mParsedBurstTimesBySwath,
    QMap<QString, QVector<qint64>>& mParsedBurstByteOffsetsBySwath,
    QMap<QString, QVector<double>>& mParsedBurstAnxTimesBySwath,
    QMap<QString, QVector<qint64>>& mParsedBurstAbsIdsBySwath,
    QMap<QString, QVector<QDateTime>>& mParsedBurstSensingTimesBySwath,
    QMap<QString, double>& mParsedNearRangeBySwath,
    QMap<QString, double>& mParsedRangeSpacingBySwath,
    QMap<QString, double>& mParsedIncidenceMidBySwath,
    SlcAnnotation* outAnnotation = nullptr,
    QMap<QString, SlcAnnotation>* outAnnotationsMap = nullptr);

// ──────────────────────────────────────────────────────────
// 构造/析构
// ──────────────────────────────────────────────────────────

Sentinel1Product::Sentinel1Product()  = default;
Sentinel1Product::~Sentinel1Product() { close(); }

void Sentinel1Product::close() {
    mIsOpen = false;
    mBands.clear();
    mOrbitVectors.clear();
    mProductType = SarProductType::Unknown;
    mOriginalPath.clear();
    mPreviewPath.clear();
    mMissionId.clear();
    mPolarizations.clear();
    mOrbitNumberAbs = 0;
    mOrbitNumberRel = 0;
}

// ──────────────────────────────────────────────────────────
// 打开产品
// ──────────────────────────────────────────────────────────

bool Sentinel1Product::open(const QString& path) {
    close();
    QFileInfo fi(path);

    if (path.endsWith(".zip", Qt::CaseInsensitive))
        return openZip(path);
    if (fi.isDir())
        return openDirectory(path);
    if (fi.fileName() == "manifest.safe") {
        QString dir = fi.absolutePath();
        if (QFileInfo::exists(dir + "/measurement"))
            return openDirectory(dir);
    }
    return false;
}

bool Sentinel1Product::openDirectory(const QString& safDir) {
    mOriginalPath = safDir;

    QString manifestPath = safDir + "/manifest.safe";
    if (!QFileInfo::exists(manifestPath)) {
        // .SAFE 可能嵌套在子目录中
        QDir d(safDir);
        QStringList subs = d.entryList({"*.SAFE"}, QDir::Dirs);
        if (!subs.isEmpty()) {
            QString sub = safDir + "/" + subs.first();
            if (QFileInfo::exists(sub + "/manifest.safe"))
                return openDirectory(sub);
        }
        return false;
    }

    if (!parseManifest(manifestPath))
        return false;

    // 解析 annotation 获取轨道/多普勒
    QString annDir = safDir + "/annotation";
    QStringList xmls;
    if (annDir.startsWith("/vsizip/")) {
        char** entries = VSIReadDir(annDir.toUtf8().constData());
        if (entries) {
            for (int i = 0; entries[i]; ++i) {
                QString f = QString::fromUtf8(entries[i]);
                if (f.endsWith(".xml", Qt::CaseInsensitive))
                    xmls.append(f);
            }
            CSLDestroy(entries);
        }
    } else if (QFileInfo::exists(annDir)) {
        QDir ad(annDir);
        xmls = ad.entryList({"*.xml"}, QDir::Files, QDir::Name);
    }
    qDebug() << "[S1Product] annotation XMLs found:" << xmls.size() << xmls;
    for (const QString& f : xmls) {
        if (!f.contains("calibration", Qt::CaseInsensitive)) {
            qDebug() << "[S1Product:stream] parsing:" << f;
            parseAnnotationStream(annDir + "/" + f);
        }
    }

    // 扫描 measurement 目录
    discoverMeasurementFiles(safDir + "/measurement");

    // 汇总传感器级元数据 (避免last-swath覆盖问题)
    finalizeSensorInfo();

    // 预览图
    QString prevDir = safDir + "/preview";
    if (QFileInfo::exists(prevDir)) {
        QDir pd(prevDir);
        QStringList imgs = pd.entryList({"*.png", "*.jpg", "*.jpeg"},
            QDir::Files);
        if (!imgs.isEmpty())
            mPreviewPath = prevDir + "/" + imgs.first();
    }

    mIsOpen = true;
    return true;
}

bool Sentinel1Product::openZip(const QString& zipPath) {
    qDebug() << "[S1Product] openZip start:" << zipPath;

    QString vsiRoot = "/vsizip/" + zipPath;
    mOriginalPath = zipPath;

    // 从文件名推断基本信息 (ZIP内路径可能不同, 先用文件名兜底)
    QString zipBase = QFileInfo(zipPath).completeBaseName();
    S1FileNameInfo fnInfo = parseS1FileName(zipBase);
    qDebug() << "[S1Product] zipBase:" << zipBase;

    // 查找 .SAFE 根目录
    QString safRoot;
    qDebug() << "[S1Product] VSIReadDir:" << vsiRoot;
    char** entries = VSIReadDir(vsiRoot.toUtf8().constData());
    qDebug() << "[S1Product] VSIReadDir done, entries:" << (entries ? "yes" : "NULL");
    if (entries) {
        for (int i = 0; entries[i]; ++i) {
            QString e = QString::fromUtf8(entries[i]);
            if (e.endsWith(".SAFE", Qt::CaseInsensitive)) {
                safRoot = vsiRoot + "/" + e;
                break;
            }
        }
        CSLDestroy(entries);
    }
    qDebug() << "[S1Product] safRoot:" << safRoot;

    if (safRoot.isEmpty()) {
        // 可能 ZIP 内没有 .SAFE 包装目录，manifest.safe 直接在根
        if (VSIStatExL((vsiRoot + "/manifest.safe").toUtf8().constData(),
                       nullptr, VSI_STAT_EXISTS_FLAG) == 0)
            safRoot = vsiRoot;
    }

    if (safRoot.isEmpty()) {
        qDebug() << "[S1Product] safRoot empty, return false";
        return false;
    }

    // 用文件名信息预设产品类型和元数据
    mSensorType = QStringLiteral("Sentinel-1");
    mMissionId   = fnInfo.missionId;
    mAcquisitionMode = fnInfo.mode;
    mProductId   = zipBase;
    mProductType = (fnInfo.productType == "SLC") ? SarProductType::SLC
                 : (fnInfo.productType.startsWith("GRD", Qt::CaseInsensitive))
                   ? SarProductType::GRD : SarProductType::Unknown;

    // 从文件名解析采集时间和极化
    // 过滤空段 (SLC 文件名有双下划线 SLC__1SDV)
    QStringList zipParts;
    for (const QString& p : zipBase.split('_')) {
        if (!p.isEmpty()) zipParts.append(p);
    }
    if (zipParts.size() >= 5) {
        QDateTime startTime = QDateTime::fromString(zipParts[4], "yyyyMMddTHHmmss");
        if (startTime.isValid())
            mSensorInfo.acquisitionStart = startTime;
    }
    // 极化: 1SDV → DV → ["VV","VH"]; 1SDH → DH → ["HH","HV"]
    QString polCode = fnInfo.polarization;
    if (polCode.size() >= 2) {
        mPolarizations.clear();
        QString abbr = polCode.right(2).toUpper(); // "DV", "DH", "SH", "SV"
        if (abbr == "DV")      mPolarizations = QStringList{"VV", "VH"};
        else if (abbr == "DH") mPolarizations = QStringList{"HH", "HV"};
        else if (abbr == "SH") mPolarizations = QStringList{"HH"};
        else if (abbr == "SV") mPolarizations = QStringList{"VV"};
        mSensorInfo.polarizations = mPolarizations;
    }

    // 轨道号
    // orbit = parts[6] (after filtering: S1A/IW/SLC/1SDV/date/date/orbit/...)
    if (zipParts.size() >= 7) {
        mOrbitNumberAbs = zipParts[6].toInt();
        mOrbitNumberRel = mOrbitNumberAbs % S1_ORBIT_REPEAT_CYCLE;
        if (mOrbitNumberRel == 0) mOrbitNumberRel = S1_ORBIT_REPEAT_CYCLE;
        mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
        mSensorInfo.relativeOrbit = mOrbitNumberRel;
        if (mSensorInfo.orbitDirection.isEmpty())
            mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1)
                ? QStringLiteral("Ascending") : QStringLiteral("Descending");
    }

    // 通过 VSI 读取 manifest 到临时文件用于 QDomDocument 解析
    QString manifestVsi = safRoot + "/manifest.safe";
    QByteArray xmlData;
    {
        VSILFILE* fp = VSIFOpenExL(manifestVsi.toUtf8().constData(), "rb", TRUE);
        if (!fp) return false;
        VSIStatBufL statBuf;
        if (VSIStatExL(manifestVsi.toUtf8().constData(), &statBuf,
                       VSI_STAT_SIZE_FLAG) == 0)
            xmlData.resize(static_cast<int>(statBuf.st_size));
        if (xmlData.isEmpty()) xmlData.resize(1024 * 1024); // fallback 1MB
        vsi_l_offset nRead = VSIFReadL(xmlData.data(), 1, xmlData.size(), fp);
        VSIFCloseL(fp);
        if (nRead == 0) return false;
        xmlData.resize(static_cast<int>(nRead));
    }

    QString tmpManifest = QDir::tempPath() + "/_s1_manifest.xml";
    QFile tmpf(tmpManifest);
    if (!tmpf.open(QIODevice::WriteOnly)) return false;
    tmpf.write(xmlData);
    tmpf.close();

    bool ok = parseManifest(tmpManifest);
    QFile::remove(tmpManifest);
    qDebug() << "[S1Product] parseManifest:" << (ok ? "OK" : "FAILED");
    if (!ok) return false;

    mOriginalPath = zipPath;

    // 解析 annotation
    QString annDir = safRoot + "/annotation";
    qDebug() << "[S1Product] annotation dir:" << annDir;
    char** annEntries = VSIReadDir(annDir.toUtf8().constData());
    qDebug() << "[S1Product] VSIReadDir annotation done, entries:" << (annEntries ? "yes" : "NULL");
    if (annEntries) {
        int xmlCount = 0;
        for (int i = 0; annEntries[i]; ++i) {
            QString e = QString::fromUtf8(annEntries[i]);
            if (e.endsWith(".xml") && !e.contains("calibration", Qt::CaseInsensitive)) {
                ++xmlCount;
                QString annVsi = annDir + "/" + e;
                qDebug() << "[S1Product]   reading:" << e;
                QByteArray aData;
                {
                    VSILFILE* fp = VSIFOpenExL(annVsi.toUtf8().constData(), "rb", TRUE);
                    if (fp) {
                        VSIStatBufL st;
                        if (VSIStatExL(annVsi.toUtf8().constData(), &st,
                                       VSI_STAT_SIZE_FLAG) == 0)
                            aData.resize(static_cast<int>(st.st_size));
                        if (aData.isEmpty()) aData.resize(1024 * 1024);
                        vsi_l_offset n = VSIFReadL(aData.data(), 1, aData.size(), fp);
                        aData.resize(static_cast<int>(n));
                        VSIFCloseL(fp);
                        qDebug() << "[S1Product]     read" << aData.size() << "bytes";
                    } else {
                        qDebug() << "[S1Product]     VSI open FAILED";
                    }
                }
                if (!aData.isEmpty()) {
                    qDebug() << "[S1Product]     parsing with SlcAnnotationReader...";
                    // 流式解析: 直接从内存数据读取, 无需临时文件
                    SlcAnnotationReader reader;
                    if (reader.openFromData(aData)) {
                        parseAnnotationFromReader(reader, mSensorInfo, mDoppler,
                            mOrbitVectors, mOrbitNumberAbs, mOrbitNumberRel,
                            mParsedLinesPerBurst, mParsedSamplesPerBurst,
                            mParsedRangeSamples,
                            mParsedAzimuthFmRateBySwath,
                            mParsedAzimuthSteeringRateBySwath,
                            mParsedAzimuthFreqBySwath,
                            mParsedLinesPerBurstBySwath,
                            mParsedSamplesPerBurstBySwath,
                            mParsedBurstStartsBySwath,
                            mParsedBurstTimesBySwath,
                            mParsedBurstByteOffsetsBySwath,
                            mParsedBurstAnxTimesBySwath,
                            mParsedBurstAbsIdsBySwath,
                            mParsedBurstSensingTimesBySwath,
                            mParsedNearRangeBySwath,
                            mParsedRangeSpacingBySwath,
                            mParsedIncidenceMidBySwath,
                            &mAnnotation, &mAnnotationsBySwath);
                        // 按波段缓存: key = "IW1/VH"
                        if (!mAnnotation.identity.swath.isEmpty())
                            mAnnotationsBySwath[mAnnotation.identity.swath + "/" + mAnnotation.identity.polarization] = mAnnotation;
                        qDebug() << "[S1Product]     parsed OK";
                    } else {
                        qDebug() << "[S1Product]     reader.openFromData FAILED";
                    }
                }
            }
        }
        CSLDestroy(annEntries);
        qDebug() << "[S1Product] processed" << xmlCount << "annotation XMLs";
    }
    qDebug() << "[S1Product] annotation parsing done";

    // 扫描 measurement
    qDebug() << "[S1Product] discoverMeasurementFiles:" << (safRoot + "/measurement");
    discoverMeasurementFiles(safRoot + "/measurement");
    qDebug() << "[S1Product] discoverMeasurementFiles done, bands:" << mBands.size();

    // 汇总传感器级元数据 (避免last-swath覆盖问题)
    finalizeSensorInfo();
    qDebug() << "[S1Product] openZip done, wavelengths=" << mSensorInfo.wavelength
             << "prf=" << mSensorInfo.prf << "nearRange=" << mSensorInfo.nearRange;

    // 预览图
    QString prevDir = safRoot + "/preview";
    char** pv = VSIReadDir(prevDir.toUtf8().constData());
    if (pv) {
        for (int i = 0; pv[i]; ++i) {
            QString e = QString::fromUtf8(pv[i]);
            if (e.endsWith(".png", Qt::CaseInsensitive)
                || e.endsWith(".jpg", Qt::CaseInsensitive)
                || e.endsWith(".jpeg", Qt::CaseInsensitive)) {
                mPreviewPath = prevDir + "/" + e;
                break;
            }
        }
        CSLDestroy(pv);
    }

    mIsOpen = true;
    return true;
}

// ──────────────────────────────────────────────────────────
// manifest.safe 解析
// ──────────────────────────────────────────────────────────

bool Sentinel1Product::parseManifest(const QString& manifestPath) {
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDomDocument doc;
    if (!doc.setContent(&file)) { file.close(); return false; }
    file.close();

    // 从目录名推断基本信息 (仅在真实 SAFE 目录时可用)
    QFileInfo fi(manifestPath);
    QString baseName = fi.dir().dirName();
    S1FileNameInfo info = parseS1FileName(baseName);
    bool fromRealSafe = baseName.endsWith(".SAFE", Qt::CaseInsensitive)
                        || QFileInfo::exists(fi.dir().absolutePath() + "/measurement");

    if (fromRealSafe || mProductType == SarProductType::Unknown) {
        mSensorType      = QStringLiteral("Sentinel-1");
        mMissionId        = info.missionId;
        mAcquisitionMode  = info.mode;
        mProductId        = baseName;
        mProductType      = (info.productType == "SLC") ? SarProductType::SLC
                          : (info.productType.startsWith("GRD", Qt::CaseInsensitive))
                            ? SarProductType::GRD
                          : mProductType;

        // 从文件名解析采集时间
        // 过滤空段 (SLC 文件名 SLC__1SDV 产生空字符串)
        QStringList parts;
        for (const QString& p : baseName.split('_')) {
            if (!p.isEmpty()) parts.append(p);
        }
        if (parts.size() >= 5) {
            QDateTime st = QDateTime::fromString(parts[4], "yyyyMMddTHHmmss");
            if (st.isValid())
                mSensorInfo.acquisitionStart = st;
        }
        // 极化
        if (info.polarization.size() >= 2) {
            QString abbr = info.polarization.right(2).toUpper();
            mPolarizations.clear();
            if (abbr == "DV")      mPolarizations = QStringList{"VV", "VH"};
            else if (abbr == "DH") mPolarizations = QStringList{"HH", "HV"};
            else if (abbr == "SH") mPolarizations = QStringList{"HH"};
            else if (abbr == "SV") mPolarizations = QStringList{"VV"};
            mSensorInfo.polarizations = mPolarizations;
        }
        // 轨道号
        if (parts.size() >= 7) {
            mOrbitNumberAbs = parts[6].toInt();
            mOrbitNumberRel = mOrbitNumberAbs % S1_ORBIT_REPEAT_CYCLE;
            if (mOrbitNumberRel == 0) mOrbitNumberRel = S1_ORBIT_REPEAT_CYCLE;
            mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
            mSensorInfo.relativeOrbit = mOrbitNumberRel;
            mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1)
                ? QStringLiteral("Ascending") : QStringLiteral("Descending");
        }
        // 采样间距在 parseAnnotation() 中从 XML 读取
    }

    // 从 XML 中提取 productType (更可靠)
    QDomElement root = doc.documentElement();
    QDomNodeList nl = root.elementsByTagName("s1sarl1:productType");
    if (nl.isEmpty())
        nl = root.elementsByTagName("productType");
    if (!nl.isEmpty()) {
        QString pt = nl.at(0).toElement().text().trimmed();
        if (!pt.isEmpty())
            mProductType = sarProductTypeFromString(pt);
    }

    // acquisition mode — 从 XML 读取 (优于文件名推断)
    nl = root.elementsByTagName("s1sarl1:mode");
    if (nl.isEmpty())
        nl = root.elementsByTagName("mode");
    if (!nl.isEmpty()) {
        QString mode = nl.at(0).toElement().text().trimmed().toUpper();
        if (!mode.isEmpty()) mAcquisitionMode = mode;
    }

    // 极化 — 遍历全部 transmitterReceiverPolarisation 元素
    // (双极化产品有多个，每个包含单一极化如 "VV" 或 "VH")
    nl = root.elementsByTagName("s1sarl1:transmitterReceiverPolarisation");
    if (nl.isEmpty())
        nl = root.elementsByTagName("transmitterReceiverPolarisation");
    mPolarizations.clear();
    for (int i = 0; i < nl.size(); ++i) {
        QString ps = nl.at(i).toElement().text().trimmed();
        if (!ps.isEmpty() && !mPolarizations.contains(ps))
            mPolarizations.append(ps);
    }
    if (mPolarizations.isEmpty()) {
        if (info.polarization.contains("DV"))
            mPolarizations = QStringList{"VV", "VH"};
        else if (info.polarization.contains("DH"))
            mPolarizations = QStringList{"HH", "HV"};
        else if (info.polarization.contains("SH"))
            mPolarizations = QStringList{"HH"};
        else if (info.polarization.contains("SV"))
            mPolarizations = QStringList{"VV"};
    }

    // 采集时间
    nl = root.elementsByTagName("s1sarl1:productFirstLineUtcTime");
    if (nl.isEmpty())
        nl = root.elementsByTagName("productFirstLineUtcTime");
    if (!nl.isEmpty()) {
        mAcquisitionStart = QDateTime::fromString(
            nl.at(0).toElement().text().trimmed(), Qt::ISODate);
    }

    // 填充 sensorInfo
    mSensorInfo.sensorType      = mSensorType;
    mSensorInfo.missionId       = mMissionId;
    mSensorInfo.acquisitionMode = mAcquisitionMode;
    mSensorInfo.productType     = mProductType;
    mSensorInfo.productId       = mProductId;
    mSensorInfo.originalPath    = fi.dir().absolutePath();
    mSensorInfo.manifestPath    = manifestPath;
    if (mAcquisitionStart.isValid())
        mSensorInfo.acquisitionStart = mAcquisitionStart;
    mSensorInfo.polarizations   = mPolarizations;
    mSensorInfo.annotationDir   = fi.dir().absolutePath() + "/annotation";
    mSensorInfo.measurementDir  = fi.dir().absolutePath() + "/measurement";

    // 波长/频率/间距 在 parseAnnotation() 中从 XML 读取，由 finalizeSensorInfo() 汇总
    return true;
}

// ──────────────────────────────────────────────────────────
// annotation XML 解析 (轨道/多普勒/几何)
// ──────────────────────────────────────────────────────────
//  QXmlStreamReader 流式解析
// ──────────────────────────────────────────────────────────

static bool parseAnnotationFromReader(SlcAnnotationReader& reader,
    SarSensorInfo& mSensorInfo, DopplerInfo& mDoppler, QList<OrbitStateVector>& mOrbitVectors,
    int& mOrbitNumberAbs, int& mOrbitNumberRel,
    int& mParsedLinesPerBurst, int& mParsedSamplesPerBurst, int& mParsedRangeSamples,
    QMap<QString, double>& mParsedAzimuthFmRateBySwath,
    QMap<QString, double>& mParsedAzimuthSteeringRateBySwath,
    QMap<QString, double>& mParsedAzimuthFreqBySwath,
    QMap<QString, int>& mParsedLinesPerBurstBySwath,
    QMap<QString, int>& mParsedSamplesPerBurstBySwath,
    QMap<QString, QVector<int>>& mParsedBurstStartsBySwath,
    QMap<QString, QVector<QDateTime>>& mParsedBurstTimesBySwath,
    QMap<QString, QVector<qint64>>& mParsedBurstByteOffsetsBySwath,
    QMap<QString, QVector<double>>& mParsedBurstAnxTimesBySwath,
    QMap<QString, QVector<qint64>>& mParsedBurstAbsIdsBySwath,
    QMap<QString, QVector<QDateTime>>& mParsedBurstSensingTimesBySwath,
    QMap<QString, double>& mParsedNearRangeBySwath,
    QMap<QString, double>& mParsedRangeSpacingBySwath,
    QMap<QString, double>& mParsedIncidenceMidBySwath,
    SlcAnnotation* outAnnotation,
    QMap<QString, SlcAnnotation>* outAnnotationsMap)
{
    SlcAnnotation ann = reader.readAll();

    QString swathName = ann.identity.swath.toUpper();
    if (swathName.isEmpty()) return false;

    if (!ann.identity.passDirection.isEmpty()) {
        QString p = ann.identity.passDirection;
        if (p == "ASCENDING")      mSensorInfo.orbitDirection = QStringLiteral("Ascending");
        else if (p == "DESCENDING") mSensorInfo.orbitDirection = QStringLiteral("Descending");
    }
    if (ann.identity.absoluteOrbitNumber > 0) {
        mOrbitNumberAbs = ann.identity.absoluteOrbitNumber;
        mOrbitNumberRel = mOrbitNumberAbs % S1_ORBIT_REPEAT_CYCLE;
        if (mOrbitNumberRel == 0) mOrbitNumberRel = S1_ORBIT_REPEAT_CYCLE;
    }
    if (!ann.dopplerEstimates.isEmpty() && !ann.dopplerEstimates[0].dataDcPoly.isEmpty())
        mDoppler.centroid = ann.dopplerEstimates[0].dataDcPoly[0];
    if (mOrbitVectors.isEmpty() && !ann.orbitList.isEmpty()) {
        for (const auto& ov : ann.orbitList) {
            OrbitStateVector sv;
            sv.utcTime = ov.utcTime; sv.relativeTime = ov.relativeTime;
            sv.x = ov.posX; sv.y = ov.posY; sv.z = ov.posZ;
            sv.vx = ov.velX; sv.vy = ov.velY; sv.vz = ov.velZ;
            mOrbitVectors.append(sv);
        }
    }
    double nearRange = ann.slantRangeTime * 299792458.0 / 2.0;
    mSensorInfo.nearRange = nearRange;
    mParsedNearRangeBySwath[swathName] = nearRange;
    if (ann.azimuthFrequency > 0) {
        mSensorInfo.prf = ann.azimuthFrequency;
        mParsedAzimuthFreqBySwath[swathName] = ann.azimuthFrequency;
    }
    if (ann.incidenceAngleMidSwath > 1.0) {
        mSensorInfo.incidenceAngleMid = ann.incidenceAngleMidSwath;
        mParsedIncidenceMidBySwath[swathName] = ann.incidenceAngleMidSwath;
    }
    if (ann.linesPerBurst > 0) {
        mParsedLinesPerBurst = ann.linesPerBurst;
        mParsedLinesPerBurstBySwath[swathName] = ann.linesPerBurst;
    }
    if (ann.samplesPerBurst > 0) {
        mParsedSamplesPerBurst = ann.samplesPerBurst;
        mParsedSamplesPerBurstBySwath[swathName] = ann.samplesPerBurst;
    }
    mParsedRangeSamples = mParsedSamplesPerBurst;

    {
        QVector<int> bStarts; QVector<QDateTime> bTimes; QVector<qint64> bOffsets;
        QVector<double> bAnxTimes; QVector<qint64> bAbsIds; QVector<QDateTime> bSensingTimes;
        for (const auto& bd : ann.burstList) {
            int fl = (bd.byteOffset > 0 && mParsedSamplesPerBurst > 0)
                ? static_cast<int>(bd.byteOffset / (mParsedSamplesPerBurst * 4LL)) : 0;
            bStarts.append(fl); bOffsets.append(bd.byteOffset);
            bTimes.append(bd.azimuthTime);
            bAnxTimes.append(bd.azimuthAnxTime);
            bAbsIds.append(bd.burstIdAbsolute);
            bSensingTimes.append(bd.sensingTime);
        }
        mParsedBurstStartsBySwath[swathName] = bStarts;
        mParsedBurstTimesBySwath[swathName] = bTimes;
        mParsedBurstByteOffsetsBySwath[swathName] = bOffsets;
        mParsedBurstAnxTimesBySwath[swathName] = bAnxTimes;
        mParsedBurstAbsIdsBySwath[swathName] = bAbsIds;
        mParsedBurstSensingTimesBySwath[swathName] = bSensingTimes;
    }
    if (ann.radarFrequency > 1e8) {
        mSensorInfo.centerFreq = ann.radarFrequency;
        mSensorInfo.wavelength = 299792458.0 / ann.radarFrequency;
    }
    if (ann.rangeSamplingRate > 1e6) {
        double rs = 299792458.0 / (2.0 * ann.rangeSamplingRate);
        mSensorInfo.rangeSpacing = rs;
        mParsedRangeSpacingBySwath[swathName] = rs;
    }
    if (ann.azimuthPixelSpacing > 0.1)
        mSensorInfo.azimuthSpacing = ann.azimuthPixelSpacing;
    if (!ann.azimuthFmRates.isEmpty() && !ann.azimuthFmRates[0].polynomial.isEmpty())
        mParsedAzimuthFmRateBySwath[swathName] = ann.azimuthFmRates[0].polynomial[0];
    if (ann.azimuthSteeringRate != 0.0)
        mParsedAzimuthSteeringRateBySwath[swathName] = ann.azimuthSteeringRate;
    mSensorInfo.rangeSamples = ann.samplesPerBurst;
    mSensorInfo.azimuthSamples = ann.linesPerBurst;
    if (mSensorInfo.nearRange > 0 && mSensorInfo.rangeSamples > 0 && mSensorInfo.rangeSpacing > 0)
        mSensorInfo.farRange = mSensorInfo.nearRange
            + (mSensorInfo.rangeSamples - 1) * mSensorInfo.rangeSpacing;
    mSensorInfo.relativeOrbit = mOrbitNumberRel;
    mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
    if (mSensorInfo.orbitDirection.isEmpty())
        mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1)
            ? QStringLiteral("Ascending") : QStringLiteral("Descending");
    mSensorInfo.orbitStateVectors = mOrbitVectors;
    mSensorInfo.doppler = mDoppler;
    if (ann.identity.startTime.isValid()) mSensorInfo.acquisitionStart = ann.identity.startTime;
    if (ann.identity.stopTime.isValid())  mSensorInfo.acquisitionStop  = ann.identity.stopTime;

    qDebug() << QStringLiteral("[S1Product:stream] swath=%1 nr=%2 aziFreq=%3 bursts=%4 fmRate=%5")
        .arg(swathName).arg(nearRange, 0, 'f', 1)
        .arg(ann.azimuthFrequency, 0, 'f', 2).arg(ann.burstList.size())
        .arg(mParsedAzimuthFmRateBySwath.value(swathName, 0.0), 0, 'f', 1);

    // 缓存第一个 annotation (用于旧兼容)
    if (outAnnotation && outAnnotation->identity.swath.isEmpty())
        *outAnnotation = ann;
    // 按波段存储
    if (outAnnotationsMap && !ann.identity.swath.isEmpty())
        (*outAnnotationsMap)[ann.identity.swath + QStringLiteral("/") + ann.identity.polarization] = ann;

    return true;
}

bool Sentinel1Product::parseAnnotationStream(const QString& annotationPath)
{
    SlcAnnotationReader reader;
    if (!reader.open(annotationPath)) {
        qWarning() << "[S1Product] SlcAnnotationReader open failed:" << annotationPath;
        return false;
    }
    return parseAnnotationFromReader(reader, mSensorInfo, mDoppler, mOrbitVectors,
        mOrbitNumberAbs, mOrbitNumberRel,
        mParsedLinesPerBurst, mParsedSamplesPerBurst, mParsedRangeSamples,
        mParsedAzimuthFmRateBySwath, mParsedAzimuthSteeringRateBySwath,
        mParsedAzimuthFreqBySwath,
        mParsedLinesPerBurstBySwath, mParsedSamplesPerBurstBySwath,
        mParsedBurstStartsBySwath, mParsedBurstTimesBySwath,
        mParsedBurstByteOffsetsBySwath,
        mParsedBurstAnxTimesBySwath,
        mParsedBurstAbsIdsBySwath,
        mParsedBurstSensingTimesBySwath,
        mParsedNearRangeBySwath, mParsedRangeSpacingBySwath,
        mParsedIncidenceMidBySwath,
        &mAnnotation, &mAnnotationsBySwath);
}

// ──────────────────────────────────────────────────────────
// 汇总传感器级元数据 (在所有 annotation XML 解析后调用)
// 解决 last-swath-wins 问题：使用中间子条带 (IW2) 作为代表值
// ──────────────────────────────────────────────────────────

void Sentinel1Product::finalizeSensorInfo()
{
    // 选择中间子条带作为传感器级代表（IW模式优先IW2, 否则取第一个可用）
    QStringList preferred = {"IW2", "IW1", "IW3"};
    QString repSwath;
    for (const QString& s : preferred) {
        if (mParsedNearRangeBySwath.contains(s)) { repSwath = s; break; }
    }
    if (repSwath.isEmpty()) {
        // 无子条带信息 (非TOPS模式), 保持 parseAnnotation 最后写入的值
        return;
    }

    // 使用代表子条带的值覆盖传感器级字段
    if (mParsedNearRangeBySwath.contains(repSwath))
        mSensorInfo.nearRange = mParsedNearRangeBySwath[repSwath];
    if (mParsedAzimuthFreqBySwath.contains(repSwath))
        mSensorInfo.prf = mParsedAzimuthFreqBySwath[repSwath];
    if (mParsedIncidenceMidBySwath.contains(repSwath))
        mSensorInfo.incidenceAngleMid = mParsedIncidenceMidBySwath[repSwath];
    if (mParsedRangeSpacingBySwath.contains(repSwath))
        mSensorInfo.rangeSpacing = mParsedRangeSpacingBySwath[repSwath];
    // 重算远距: nearRange 被覆盖为 IW2 后, farRange 也要跟着更新
    if (mSensorInfo.nearRange > 0 && mSensorInfo.rangeSamples > 0 && mSensorInfo.rangeSpacing > 0)
        mSensorInfo.farRange = mSensorInfo.nearRange
            + (mSensorInfo.rangeSamples - 1) * mSensorInfo.rangeSpacing;

    // 入射角近/远端: 从 IW1 近距和 IW3 远距计算
    // 使用简化的球面地球模型: cos(inc) = sqrt((H+R)^2 + R^2 - Re^2) / (2*(H+R)*R)
    const double Re = 6371000.0; // 地球平均半径 (m)
    if (mSensorInfo.incidenceAngleNear <= 0 && mParsedNearRangeBySwath.contains("IW1")) {
        double nearR = mParsedNearRangeBySwath["IW1"];
        if (nearR > 0) {
            double orbitH = 693000.0; // S1 approx orbit height (m)
            mSensorInfo.incidenceAngleNear = std::acos(std::max(-1.0, std::min(1.0,
                (orbitH * orbitH + nearR * nearR - Re * Re) / (2.0 * orbitH * nearR)))
                ) * 57.295779513;
        }
    }
    if (mSensorInfo.incidenceAngleFar <= 0 && mParsedNearRangeBySwath.contains("IW3")
        && mParsedSamplesPerBurstBySwath.contains("IW3")) {
        double nearR3 = mParsedNearRangeBySwath["IW3"];
        double rangeSpacing = mParsedRangeSpacingBySwath.value("IW3", mSensorInfo.rangeSpacing);
        int samples3 = mParsedSamplesPerBurstBySwath["IW3"];
        double farR3 = nearR3 + (samples3 - 1) * rangeSpacing;
        double orbitH = 693000.0;
        mSensorInfo.incidenceAngleFar = std::acos(std::max(-1.0, std::min(1.0,
            (orbitH * orbitH + farR3 * farR3 - Re * Re) / (2.0 * orbitH * farR3)))
            ) * 57.295779513;
    }

    // 回退: 仍为0则用 mid±5°
    if (mSensorInfo.incidenceAngleNear <= 0 && mSensorInfo.incidenceAngleMid > 0)
        mSensorInfo.incidenceAngleNear = mSensorInfo.incidenceAngleMid - 5.0;
    if (mSensorInfo.incidenceAngleFar <= 0 && mSensorInfo.incidenceAngleMid > 0)
        mSensorInfo.incidenceAngleFar = mSensorInfo.incidenceAngleMid + 5.0;
}

// ──────────────────────────────────────────────────────────
// 波段发现
// ──────────────────────────────────────────────────────────

void Sentinel1Product::discoverMeasurementFiles(const QString& measurementDir) {
    mBands.clear();

    // /vsizip 路径用 VSI, 普通路径用 QDir
    QStringList tifFiles;
    if (measurementDir.startsWith("/vsizip/")) {
        char** entries = VSIReadDir(measurementDir.toUtf8().constData());
        if (entries) {
            for (int i = 0; entries[i]; ++i)
                tifFiles.append(QString::fromUtf8(entries[i]));
            CSLDestroy(entries);
        }
    } else if (QFileInfo::exists(measurementDir)) {
        QDir d(measurementDir);
        tifFiles = d.entryList({"*.tiff", "*.tif"}, QDir::Files);
    }

    for (const QString& tf : tifFiles) {
        SarBandDescriptor b;
        b.rasterPath = measurementDir + "/" + tf;
        b.index = mBands.size();

        // 产品类型决定数据格式，避免逐个 GDALOpen（/vsizip 解压开销）
        b.isComplex = (mProductType == SarProductType::SLC);
        b.dataType  = b.isComplex ? QStringLiteral("CInt16")
                                  : QStringLiteral("UInt16");

        // 从文件名推断极化和子条带
        S1FileNameInfo info = parseS1FileName(tf);
        if (info.polarization.size() == 2)
            b.polarization = info.polarization.toUpper();
        b.subSwath = info.subSwath.toUpper();

        // 兜底: 从文件名关键字推断极化
        if (b.polarization.isEmpty()) {
            QString l = tf.toLower();
            if (l.contains("-vv"))       b.polarization = "VV";
            else if (l.contains("-vh"))   b.polarization = "VH";
            else if (l.contains("-hh"))   b.polarization = "HH";
            else if (l.contains("-hv"))   b.polarization = "HV";
        }

        // 传入 burst 信息 (每子条带独立)
        b.linesPerBurst   = mParsedLinesPerBurstBySwath.value(b.subSwath, mParsedLinesPerBurst);
        b.burstCount      = b.linesPerBurst > 0
                          ? mParsedBurstStartsBySwath.value(b.subSwath, mParsedBurstStarts).size() : 0;
        b.burstStartLines = mParsedBurstStartsBySwath.value(b.subSwath, mParsedBurstStarts);
        b.burstAzimuthTimes = mParsedBurstTimesBySwath.value(b.subSwath, mParsedBurstAzimuthTimes);
        b.burstByteOffsets     = mParsedBurstByteOffsetsBySwath.value(b.subSwath, mParsedBurstByteOffsets);
        b.burstAzimuthAnxTimes = mParsedBurstAnxTimesBySwath.value(b.subSwath, QVector<double>());
        b.burstAbsoluteIds     = mParsedBurstAbsIdsBySwath.value(b.subSwath, QVector<qint64>());
        b.burstSensingTimes    = mParsedBurstSensingTimesBySwath.value(b.subSwath, QVector<QDateTime>());
        b.samplesPerBurst   = mParsedSamplesPerBurstBySwath.value(b.subSwath, mParsedSamplesPerBurst);
        b.nearRange         = mParsedNearRangeBySwath.value(b.subSwath, 0.0);
        b.rangeSpacing      = mSensorInfo.rangeSpacing;  // 所有 IW 共享
        b.azimuthFmRate      = mParsedAzimuthFmRateBySwath.value(b.subSwath, 0.0);
        b.azimuthSteeringRate = mParsedAzimuthSteeringRateBySwath.value(b.subSwath, 0.0);
        b.azimuthFrequency    = mParsedAzimuthFreqBySwath.value(b.subSwath, 0.0);
        b.azimuthSpacing     = mSensorInfo.azimuthSpacing;  // 所有 IW 共享
        qDebug() << QStringLiteral("[S1Product] band %1 L=%2 bursts=%3 aziFreq=%4 Hz fmRate=%5 steerRate=%6")
            .arg(b.subSwath).arg(b.linesPerBurst).arg(b.burstCount)
            .arg(b.azimuthFrequency, 0, 'f', 2)
            .arg(b.azimuthFmRate, 0, 'f', 1)
            .arg(b.azimuthSteeringRate, 0, 'f', 3);

        // 填充 burst 时间范围
        if (!b.burstAzimuthTimes.isEmpty()) {
            b.burstTimeStart = b.burstAzimuthTimes.first();
            b.burstTimeStop  = b.burstAzimuthTimes.last();
        }
        // 填充 rasterSize (TOPSAR: burst尺寸; 非TOPSAR: 全图)
        if (b.burstCount > 0 && b.samplesPerBurst > 0 && b.linesPerBurst > 0)
            b.rasterSize = QSize(b.samplesPerBurst, b.linesPerBurst * b.burstCount);
        else if (mSensorInfo.rangeSamples > 0 && mSensorInfo.azimuthSamples > 0)
            b.rasterSize = QSize(mSensorInfo.rangeSamples, mSensorInfo.azimuthSamples);

        mBands.append(b);
    }

    // 按极化 + 子条带排序
    std::sort(mBands.begin(), mBands.end(),
        [](const SarBandDescriptor& a, const SarBandDescriptor& b) {
            if (a.polarization != b.polarization)
                return a.polarization < b.polarization;
            return a.subSwath < b.subSwath;
        });
    for (int i = 0; i < mBands.size(); ++i)
        mBands[i].index = i;
}

QList<SarBandDescriptor> Sentinel1Product::bandsByPolarization(
        const QString& pol) const {
    QList<SarBandDescriptor> result;
    for (const auto& b : mBands) {
        if (b.polarization.compare(pol, Qt::CaseInsensitive) == 0)
            result.append(b);
    }
    return result;
}

// ──────────────────────────────────────────────────────────
// 复数数据读取
// ──────────────────────────────────────────────────────────

QVector<std::complex<float>> Sentinel1Product::readComplexSamples(
        int bandIndex, int x0, int y0, int w, int h) {
    QVector<std::complex<float>> data;
    if (bandIndex < 0 || bandIndex >= mBands.size())
        return data;

    const SarBandDescriptor& b = mBands[bandIndex];
    GDALDatasetH hDS = GDALOpen(b.rasterPath.toUtf8().constData(), GA_ReadOnly);
    if (!hDS) return data;

    data.resize(w * h);
    CPLErr err = GDALRasterIO(GDALGetRasterBand(hDS, 1), GF_Read,
        x0, y0, w, h, reinterpret_cast<CFloat32*>(data.data()), w, h, GDT_CFloat32, 0, 0);
    GDALClose(hDS);

    if (err != CE_None)
        data.clear();
    return data;
}

// ──────────────────────────────────────────────────────────
// 文件名解析
// ──────────────────────────────────────────────────────────

Sentinel1Product::S1FileNameInfo Sentinel1Product::parseS1FileName(
        const QString& fileName) {
    S1FileNameInfo info;
    QString base = QFileInfo(fileName).completeBaseName();

    // SAFE 目录名: S1A_IW_SLC__1SDV_2023...
    if (base.startsWith("S1", Qt::CaseInsensitive) && base.contains('_')) {
        QStringList parts = base.split('_', Qt::SkipEmptyParts);
        if (parts.size() >= 4) {
            info.missionId    = parts[0].toUpper();
            info.mode         = parts[1].toUpper();
            info.productType  = parts[2].toUpper();
            if (parts.size() >= 5)
                info.resolution = parts[3].toUpper();
            if (parts.size() >= 6) {
                info.polarization = parts[4].toUpper();
                // 1SDV → DV, 1SDH → DH, 1SSH → SH, 1SSV → SV
                if (info.polarization.size() >= 3)
                    info.polarization = info.polarization.right(2);
            }
        }
    }
    // measurement 文件名: s1a-iw1-slc-vv-...
    else if (base.startsWith("s1", Qt::CaseInsensitive) && base.contains('-')) {
        QStringList parts = base.split('-', Qt::SkipEmptyParts);
        if (parts.size() >= 4) {
            info.missionId    = parts[0].toUpper();
            info.subSwath     = parts[1].toUpper();
            info.productType  = parts[2].toUpper();
            info.polarization = parts[3].toUpper();
        }
    }

    return info;
}


