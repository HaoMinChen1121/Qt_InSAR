#include "Sentinel1Product.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>
#include <algorithm>

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_port.h>
#include <cpl_vsi.h>
#include <cpl_string.h>

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
    if (QFileInfo::exists(annDir)) {
        QDir ad(annDir);
        QStringList xmls = ad.entryList({"*.xml"}, QDir::Files, QDir::Name);
        for (const QString& f : xmls) {
            if (!f.contains("calibration", Qt::CaseInsensitive))
                parseAnnotation(annDir + "/" + f);
        }
    }

    // 扫描 measurement 目录
    discoverMeasurementFiles(safDir + "/measurement");

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
    QString vsiRoot = "/vsizip/" + zipPath;
    mOriginalPath = zipPath;

    // 从文件名推断基本信息 (ZIP内路径可能不同, 先用文件名兜底)
    QString zipBase = QFileInfo(zipPath).completeBaseName();
    S1FileNameInfo fnInfo = parseS1FileName(zipBase);

    // 查找 .SAFE 根目录
    QString safRoot;
    char** entries = VSIReadDir(vsiRoot.toUtf8().constData());
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

    if (safRoot.isEmpty()) {
        // 可能 ZIP 内没有 .SAFE 包装目录，manifest.safe 直接在根
        if (VSIStatExL((vsiRoot + "/manifest.safe").toUtf8().constData(),
                       nullptr, VSI_STAT_EXISTS_FLAG) == 0)
            safRoot = vsiRoot;
    }

    if (safRoot.isEmpty())
        return false;

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
        mOrbitNumberRel = mOrbitNumberAbs % 175;
        if (mOrbitNumberRel == 0) mOrbitNumberRel = 175;
        mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
        mSensorInfo.relativeOrbit = mOrbitNumberRel;
        mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1)
            ? QStringLiteral("Ascending") : QStringLiteral("Descending");
    }

    // Sentinel-1 IW GRD 近似采样间距
    if (mAcquisitionMode == "IW") {
        if (mProductType == SarProductType::GRD) {
            mSensorInfo.rangeSpacing   = 10.0;
            mSensorInfo.azimuthSpacing = 10.0;
        } else {
            mSensorInfo.rangeSpacing   = 2.3;
            mSensorInfo.azimuthSpacing = 14.0;
        }
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
    if (!ok) return false;

    mOriginalPath = zipPath;

    // 解析 annotation
    QString annDir = safRoot + "/annotation";
    char** annEntries = VSIReadDir(annDir.toUtf8().constData());
    if (annEntries) {
        for (int i = 0; i < 20 && annEntries[i]; ++i) {
            QString e = QString::fromUtf8(annEntries[i]);
            if (e.endsWith(".xml") && !e.contains("calibration", Qt::CaseInsensitive)) {
                QString annVsi = annDir + "/" + e;
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
                    }
                }
                if (!aData.isEmpty()) {
                    QString tmpAnn = QDir::tempPath() + "/_s1_ann.xml";
                    QFile af(tmpAnn);
                    if (af.open(QIODevice::WriteOnly)) {
                        af.write(aData);
                        af.close();
                        parseAnnotation(tmpAnn);
                        QFile::remove(tmpAnn);
                        break;
                    }
                }
            }
        }
        CSLDestroy(annEntries);
    }

    // 扫描 measurement
    discoverMeasurementFiles(safRoot + "/measurement");

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
            mOrbitNumberRel = mOrbitNumberAbs % 175;
            if (mOrbitNumberRel == 0) mOrbitNumberRel = 175;
            mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
            mSensorInfo.relativeOrbit = mOrbitNumberRel;
            mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1)
                ? QStringLiteral("Ascending") : QStringLiteral("Descending");
        }
        // 近似采样间距
        if (mAcquisitionMode == "IW") {
            if (mProductType == SarProductType::GRD) {
                mSensorInfo.rangeSpacing   = 10.0;
                mSensorInfo.azimuthSpacing = 10.0;
            } else {
                mSensorInfo.rangeSpacing   = 2.3;
                mSensorInfo.azimuthSpacing = 14.0;
            }
        }
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

    // 波长: Sentinel-1 C-band = 5.405 GHz → 0.0555 m
    mSensorInfo.wavelength = 0.05546576;
    mSensorInfo.centerFreq = 5405000000.0;

    return true;
}

// ──────────────────────────────────────────────────────────
// annotation XML 解析 (轨道/多普勒/几何)
// ──────────────────────────────────────────────────────────

bool Sentinel1Product::parseAnnotation(const QString& annotationPath) {
    QFile file(annotationPath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDomDocument doc;
    if (!doc.setContent(&file)) { file.close(); return false; }
    file.close();

    QDomElement root = doc.documentElement();

    // 轨道号
    QDomNodeList nl = root.elementsByTagName("orbitNumber");
    if (!nl.isEmpty()) {
        mOrbitNumberAbs = nl.at(0).toElement().text().toInt();
        mOrbitNumberRel = mOrbitNumberAbs % 175;
        if (mOrbitNumberRel == 0) mOrbitNumberRel = 175;
    }

    // 多普勒质心
    nl = root.elementsByTagName("dcEstimate");
    if (!nl.isEmpty()) {
        QDomElement dce = nl.at(0).toElement();
        QDomElement coeff = dce.firstChildElement("dataDcPolynomial")
                              .firstChildElement("coefficient");
        if (!coeff.isNull())
            mDoppler.centroid = coeff.text().toDouble();
    }

    // 轨道状态向量
    nl = root.elementsByTagName("orbitStateVector");
    for (int i = 0; i < nl.size() && i < 50; ++i) {
        QDomElement osv = nl.at(i).toElement();
        OrbitStateVector sv;
        sv.time = osv.firstChildElement("osvTime").text().toDouble();
        sv.x = osv.firstChildElement("xPos").text().toDouble();
        sv.y = osv.firstChildElement("yPos").text().toDouble();
        sv.z = osv.firstChildElement("zPos").text().toDouble();
        sv.vx = osv.firstChildElement("xVel").text().toDouble();
        sv.vy = osv.firstChildElement("yVel").text().toDouble();
        sv.vz = osv.firstChildElement("zVel").text().toDouble();
        mOrbitVectors.append(sv);
    }

    // 近距
    nl = root.elementsByTagName("slantRangeTime");
    if (!nl.isEmpty())
        mSensorInfo.nearRange = nl.at(0).toElement().text().toDouble()
                                * 299792458.0 / 2.0;

    // PRF
    nl = root.elementsByTagName("prf");
    if (!nl.isEmpty())
        mSensorInfo.prf = nl.at(0).toElement().text().toDouble();

    // 入射角
    nl = root.elementsByTagName("incidenceAngle");
    if (!nl.isEmpty()) {
        double inc = nl.at(0).toElement().text().toDouble();
        mSensorInfo.incidenceAngleMid   = inc;
        mSensorInfo.incidenceAngleNear  = inc - 5.0;
        mSensorInfo.incidenceAngleFar   = inc + 5.0;
    }

    // 采样数
    nl = root.elementsByTagName("samplesPerBurst");
    if (!nl.isEmpty())
        mSensorInfo.rangeSamples  = nl.at(0).toElement().text().toInt();
    nl = root.elementsByTagName("linesPerBurst");
    if (!nl.isEmpty())
        mSensorInfo.azimuthSamples = nl.at(0).toElement().text().toInt();

    // 远距 = 近距 + (距离向采样数 - 1) × 距离向采样间距
    if (mSensorInfo.nearRange > 0 && mSensorInfo.rangeSamples > 0
        && mSensorInfo.rangeSpacing > 0)
        mSensorInfo.farRange = mSensorInfo.nearRange
            + (mSensorInfo.rangeSamples - 1) * mSensorInfo.rangeSpacing;

    // 更新 sensorInfo
    mSensorInfo.relativeOrbit = mOrbitNumberRel;
    mSensorInfo.absoluteOrbit = mOrbitNumberAbs;
    mSensorInfo.orbitDirection = (mOrbitNumberRel % 2 == 1) ?
        QStringLiteral("Ascending") : QStringLiteral("Descending");
    mSensorInfo.orbitStateVectors = mOrbitVectors;
    mSensorInfo.doppler = mDoppler;

    return true;
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
        x0, y0, w, h, data.data(), w, h, GDT_CFloat32, 0, 0);
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

// ──────────────────────────────────────────────────────────
// XML 辅助
// ──────────────────────────────────────────────────────────

QString Sentinel1Product::xmlFirstElementText(
        const QDomElement& parent, const QString& tagName) const {
    QDomElement el = parent.firstChildElement(tagName);
    return el.isNull() ? QString() : el.text().trimmed();
}

double Sentinel1Product::xmlFirstElementDouble(
        const QDomElement& parent, const QString& tagName) const {
    return xmlFirstElementText(parent, tagName).toDouble();
}
