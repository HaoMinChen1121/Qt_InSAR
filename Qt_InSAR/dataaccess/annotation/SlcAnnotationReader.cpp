#include "SlcAnnotationReader.h"

#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>
#include <QBuffer>

#include <cpl_vsi.h>
#include <gdal_priv.h>

// ═══════════════════════════════════════════════════════════
//  构造/析构
// ═══════════════════════════════════════════════════════════

SlcAnnotationReader::~SlcAnnotationReader() { close(); }

void SlcAnnotationReader::close()
{
    if (mXml) { delete mXml; mXml = nullptr; }
    if (mFile) { delete mFile; mFile = nullptr; }
    mDevice = nullptr;
    mData.clear();
    mFilePath.clear();
}

// ═══════════════════════════════════════════════════════════
//  打开
// ═══════════════════════════════════════════════════════════

bool SlcAnnotationReader::open(const QString& filePath)
{
    close();

    // 对于 /vsizip/ 等 VSI 路径, 先用 VSI 读取到内存
    if (filePath.startsWith("/vsi", Qt::CaseInsensitive)) {
        VSILFILE* fp = VSIFOpenExL(filePath.toUtf8().constData(), "rb", TRUE);
        if (!fp) return false;
        VSIStatBufL st;
        mData.resize(1048576); // 默认 1MB
        if (VSIStatExL(filePath.toUtf8().constData(), &st, VSI_STAT_SIZE_FLAG) == 0
            && st.st_size > 0)
            mData.resize(static_cast<int>(st.st_size));
        vsi_l_offset n = VSIFReadL(mData.data(), 1, mData.size(), fp);
        VSIFCloseL(fp);
        if (n == 0) return false;
        mData.resize(static_cast<int>(n));
        return openFromData(mData);
    }

    // 本地文件
    QFile* f = new QFile(filePath);
    if (!f->open(QIODevice::ReadOnly | QIODevice::Text)) {
        delete f;
        return false;
    }
    mFile   = f;
    mDevice = f;
    mXml    = new QXmlStreamReader(mDevice);
    mFilePath = filePath;
    return true;
}

bool SlcAnnotationReader::openFromData(const QByteArray& data)
{
    close();
    mData = data;
    auto* buf = new QBuffer(&mData);
    buf->open(QIODevice::ReadOnly);
    mDevice = buf;
    mXml    = new QXmlStreamReader(mDevice);
    return true;
}

void SlcAnnotationReader::reset()
{
    if (!mXml) return;
    // QXmlStreamReader 不支持 seek; 重新创建
    delete mXml;
    mXml = nullptr;
    if (mFile) {
        mFile->seek(0);
        mXml = new QXmlStreamReader(mFile);
    } else if (!mData.isEmpty()) {
        // 先释放旧的 QBuffer (避免 readAll 多次 reset 泄漏)
        if (mDevice && mDevice != mFile) { delete mDevice; mDevice = nullptr; }
        auto* buf = new QBuffer(&mData);
        buf->open(QIODevice::ReadOnly);
        mDevice = buf;
        mXml    = new QXmlStreamReader(mDevice);
    }
}

// ═══════════════════════════════════════════════════════════
//  流式辅助
// ═══════════════════════════════════════════════════════════

bool SlcAnnotationReader::seekToElement(const QString& elementName)
{
    if (!mXml) return false;
    while (!mXml->atEnd() && !mXml->hasError()) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == elementName)
            return true;
    }
    if (mXml->hasError())
        qWarning() << "[SlcReader] XML error seeking" << elementName
                   << ":" << mXml->errorString() << "at line" << mXml->lineNumber();
    return false;
}

QString SlcAnnotationReader::readElementText()
{
    if (!mXml || !mXml->isStartElement()) return QString();
    return mXml->readElementText().trimmed();
}

void SlcAnnotationReader::skipSubtree()
{
    if (!mXml || !mXml->isStartElement()) return;
    int depth = 1;
    while (depth > 0 && !mXml->atEnd()) {
        mXml->readNext();
        if (mXml->isStartElement()) ++depth;
        else if (mXml->isEndElement()) --depth;
    }
}

QVector<double> SlcAnnotationReader::parsePolynomial(const QString& text)
{
    QVector<double> coeffs;
    const auto parts = text.split(' ', Qt::SkipEmptyParts);
    for (const auto& p : parts) {
        bool ok = false;
        double v = p.toDouble(&ok);
        if (ok) coeffs.append(v);
    }
    return coeffs;
}

QDateTime SlcAnnotationReader::parseIso8601(const QString& s)
{
    if (s.isEmpty()) return QDateTime();
    // Qt5 支持 yyyy-MM-ddTHH:mm:ss.zzz (毫秒)
    // 微秒格式: yyyy-MM-ddTHH:mm:ss.zzzzzz — Qt5 需要截断到3位 + 手动微秒
    QDateTime dt = QDateTime::fromString(s.left(23), "yyyy-MM-ddTHH:mm:ss.zzz");
    if (dt.isValid()) {
        // 提取微秒部分 (位置 23+)
        int dotIdx = s.indexOf('.');
        if (dotIdx > 0 && s.length() > dotIdx + 4) {
            int usec = s.mid(dotIdx + 4, 3).toInt() * 1000; // 微秒
            if (s.length() > dotIdx + 7)
                usec += s.mid(dotIdx + 7, 3).toInt(); // 纳秒→微秒舍入
            dt = dt.addMSecs(usec / 1000.0);
        }
    }
    return dt;
}

// ═══════════════════════════════════════════════════════════
//  第1层：快速扫描
// ═══════════════════════════════════════════════════════════

ProductIdentity SlcAnnotationReader::readIdentity()
{
    ProductIdentity id;
    if (!mXml) return id;

    reset();
    if (!seekToElement("adsHeader")) return id;

    while (!(mXml->isEndElement() && mXml->name() == "adsHeader")) {
        mXml->readNext();
        if (mXml->isStartElement()) {
            QStringRef n = mXml->name();
            if (n == "missionId")            id.missionId = readElementText();
            else if (n == "productType")      id.productType = readElementText();
            else if (n == "polarisation")     id.polarization = readElementText();
            else if (n == "mode")             id.mode = readElementText();
            else if (n == "swath")            id.swath = readElementText();
            else if (n == "startTime")        id.startTime = parseIso8601(readElementText());
            else if (n == "stopTime")         id.stopTime  = parseIso8601(readElementText());
            else if (n == "absoluteOrbitNumber") id.absoluteOrbitNumber = readElementText().toInt();
            else if (n == "missionDataTakeId")   id.missionDataTakeId = readElementText();
        }
    }
    return id;
}

QString SlcAnnotationReader::readPassDirection()
{
    if (!mXml) return QString();

    reset();
    // 定位到 generalAnnotation → productInformation → pass
    if (!seekToElement("generalAnnotation")) return QString();

    // 先进入 productInformation (pass 的正确父元素)
    if (!seekToElement("productInformation")) return QString();

    while (!(mXml->isEndElement() && mXml->name() == "productInformation")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "pass")
            return readElementText();
    }
    return QString();
}

// ═══════════════════════════════════════════════════════════
//  第2层：核心加载
// ═══════════════════════════════════════════════════════════

QVector<OrbitVector> SlcAnnotationReader::readOrbitVectors()
{
    reset();
    return readOrbitVectorsNoReset();
}

QVector<OrbitVector> SlcAnnotationReader::readOrbitVectorsNoReset()
{
    QVector<OrbitVector> result;
    if (!mXml) return result;

    // 定位到 orbitList
    while (seekToElement("orbitList")) {
        QXmlStreamAttributes attrs = mXml->attributes();
        int expected = attrs.value("count").toInt();

        QDateTime refTime;
        while (!(mXml->isEndElement() && mXml->name() == "orbitList")) {
            mXml->readNext();
            if (mXml->isStartElement() && mXml->name() == "orbit") {
                OrbitVector ov;
                while (!(mXml->isEndElement() && mXml->name() == "orbit")) {
                    mXml->readNext();
                    if (mXml->isStartElement()) {
                        QStringRef n = mXml->name();
                        if (n == "time") {
                            ov.utcTime = parseIso8601(readElementText());
                            if (!refTime.isValid()) refTime = ov.utcTime;
                            ov.relativeTime = refTime.secsTo(ov.utcTime);
                        } else if (n == "frame") {
                            ov.frame = readElementText();
                        } else if (n == "position") {
                            while (!(mXml->isEndElement() && mXml->name() == "position")) {
                                mXml->readNext();
                                if (mXml->isStartElement()) {
                                    QStringRef cn = mXml->name();
                                    double v = readElementText().toDouble();
                                    if (cn == "x") ov.posX = v;
                                    else if (cn == "y") ov.posY = v;
                                    else if (cn == "z") ov.posZ = v;
                                }
                            }
                        } else if (n == "velocity") {
                            while (!(mXml->isEndElement() && mXml->name() == "velocity")) {
                                mXml->readNext();
                                if (mXml->isStartElement()) {
                                    QStringRef cn = mXml->name();
                                    double v = readElementText().toDouble();
                                    if (cn == "x") ov.velX = v;
                                    else if (cn == "y") ov.velY = v;
                                    else if (cn == "z") ov.velZ = v;
                                }
                            }
                        }
                    }
                }
                result.append(ov);
            }
        }

        // 验证计数
        if (expected > 0 && result.size() != expected)
            qWarning() << "[SlcReader] orbitList count mismatch: expected" << expected
                       << "got" << result.size();

        // 只解析第一个 orbitList (有多个子条带信息时只读第一个)
        break;
    }
    return result;
}

double SlcAnnotationReader::readRadarFrequency()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("radarFrequency")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readRangeSamplingRate()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("rangeSamplingRate")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readAzimuthSteeringRate()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("azimuthSteeringRate")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readSlantRangeTime()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("slantRangeTime")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readIncidenceAngleMidSwath()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("incidenceAngleMidSwath")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readAzimuthFrequency()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("azimuthFrequency")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readAzimuthPixelSpacing()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("azimuthPixelSpacing")) return 0.0;
    return readElementText().toDouble();
}

double SlcAnnotationReader::readZeroDopMinusAcqTime()
{
    if (!mXml) return 0.0;
    reset();
    if (!seekToElement("zeroDopMinusAcqTime")) return 0.0;
    return readElementText().toDouble();
}

int SlcAnnotationReader::readNumberOfSamples()
{
    if (!mXml) return 0;
    reset();
    if (!seekToElement("numberOfSamples")) return 0;
    return readElementText().toInt();
}

int SlcAnnotationReader::readNumberOfLines()
{
    if (!mXml) return 0;
    reset();
    if (!seekToElement("numberOfLines")) return 0;
    return readElementText().toInt();
}

int SlcAnnotationReader::readLinesPerBurst()
{
    if (!mXml) return 0;
    reset();
    if (!seekToElement("linesPerBurst")) return 0;
    return readElementText().toInt();
}

int SlcAnnotationReader::readSamplesPerBurst()
{
    if (!mXml) return 0;
    reset();
    if (!seekToElement("samplesPerBurst")) return 0;
    return readElementText().toInt();
}

QVector<BurstData> SlcAnnotationReader::readBurstTiming()
{
    reset();
    return readBurstTimingNoReset();
}

QVector<BurstData> SlcAnnotationReader::readBurstTimingNoReset()
{
    QVector<BurstData> result;
    if (!mXml) return result;
    // 如果 reader 已经在 burstList 开始标签上, 不要再 seek(会跨过去)
    if (!(mXml->isStartElement() && mXml->name() == QStringLiteral("burstList"))) {
        if (!seekToElement("burstList")) return result;
    }

    while (!(mXml->isEndElement() && mXml->name() == "burstList")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "burst") {
            BurstData bd;
            while (!(mXml->isEndElement() && mXml->name() == "burst")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    if (n == "azimuthTime")
                        bd.azimuthTime = parseIso8601(readElementText());
                    else if (n == "azimuthAnxTime")
                        bd.azimuthAnxTime = readElementText().toDouble();
                    else if (n == "sensingTime")
                        bd.sensingTime = parseIso8601(readElementText());
                    else if (n == "byteOffset")
                        bd.byteOffset = readElementText().toLongLong();
                    else if (n == "burstId") {
                        // <burstId absolute="139254617">21637</burstId>
                        auto attrs = mXml->attributes();
                        bd.burstIdAbsolute = attrs.value("absolute").toLongLong();
                        bd.burstIdRelative = readElementText().toInt();
                    } else if (n == "firstValidSample" || n == "lastValidSample") {
                        // 跳过大数组 (各 ~1494 int), 不存储
                        readElementText();
                    }
                }
            }
            result.append(bd);
        }
    }
    return result;
}

QVector<GeolocationPoint> SlcAnnotationReader::readGeolocationGrid()
{
    reset();
    return readGeolocationGridNoReset();
}

QVector<GeolocationPoint> SlcAnnotationReader::readGeolocationGridNoReset()
{
    QVector<GeolocationPoint> result;
    if (!mXml) return result;
    if (!seekToElement("geolocationGridPointList")) return result;

    while (!(mXml->isEndElement() && mXml->name() == "geolocationGridPointList")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "geolocationGridPoint") {
            GeolocationPoint gp;
            while (!(mXml->isEndElement() && mXml->name() == "geolocationGridPoint")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    QString  v = readElementText();
                    if (n == "azimuthTime")         gp.azimuthTime = parseIso8601(v);
                    else if (n == "slantRangeTime")  gp.slantRangeTime = v.toDouble();
                    else if (n == "line")            gp.line  = v.toInt();
                    else if (n == "pixel")           gp.pixel = v.toInt();
                    else if (n == "latitude")        gp.latitude  = v.toDouble();
                    else if (n == "longitude")       gp.longitude = v.toDouble();
                    else if (n == "height")          gp.height = v.toDouble();
                    else if (n == "incidenceAngle")  gp.incidenceAngle = v.toDouble();
                    else if (n == "elevationAngle")  gp.elevationAngle = v.toDouble();
                }
            }
            result.append(gp);
        }
    }
    return result;
}

QVector<DopplerEstimate> SlcAnnotationReader::readDopplerCentroid()
{
    reset();
    return readDopplerCentroidNoReset();
}

QVector<DopplerEstimate> SlcAnnotationReader::readDopplerCentroidNoReset()
{
    QVector<DopplerEstimate> result;
    if (!mXml) return result;
    if (!seekToElement("dcEstimateList")) return result;

    while (!(mXml->isEndElement() && mXml->name() == "dcEstimateList")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "dcEstimate") {
            DopplerEstimate de;
            while (!(mXml->isEndElement() && mXml->name() == "dcEstimate")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    if (n == "azimuthTime") {
                        de.azimuthTime = parseIso8601(readElementText());
                    } else if (n == "t0") {
                        de.t0 = readElementText().toDouble();
                    } else if (n == "geometryDcPolynomial") {
                        de.geometryDcPoly = parsePolynomial(readElementText());
                    } else if (n == "dataDcPolynomial") {
                        de.dataDcPoly = parsePolynomial(readElementText());
                    } else if (n == "dataDcRmsError") {
                        de.dataDcRmsError = readElementText().toDouble();
                    } else if (n == "dataDcRmsErrorAboveThreshold") {
                        de.rmsErrorAboveThreshold = (readElementText() == "true");
                    } else if (n == "fineDceList") {
                        while (!(mXml->isEndElement() && mXml->name() == "fineDceList")) {
                            mXml->readNext();
                            if (mXml->isStartElement() && mXml->name() == "fineDce") {
                                double srTime = 0.0, freq = 0.0;
                                while (!(mXml->isEndElement() && mXml->name() == "fineDce")) {
                                    mXml->readNext();
                                    if (mXml->isStartElement()) {
                                        QStringRef fn = mXml->name();
                                        double v = readElementText().toDouble();
                                        if (fn == "slantRangeTime") srTime = v;
                                        else if (fn == "frequency") freq = v;
                                    }
                                }
                                de.fineDce.append({srTime, freq});
                            }
                        }
                    }
                }
            }
            result.append(de);
        }
    }
    return result;
}

QVector<AzimuthFmRate> SlcAnnotationReader::readAzimuthFmRates()
{
    reset();
    return readAzimuthFmRatesNoReset();
}

QVector<AzimuthFmRate> SlcAnnotationReader::readAzimuthFmRatesNoReset()
{
    QVector<AzimuthFmRate> result;
    if (!mXml) return result;
    if (!seekToElement("azimuthFmRateList")) return result;

    while (!(mXml->isEndElement() && mXml->name() == "azimuthFmRateList")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "azimuthFmRate") {
            AzimuthFmRate afr;
            while (!(mXml->isEndElement() && mXml->name() == "azimuthFmRate")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    if (n == "azimuthTime")
                        afr.azimuthTime = parseIso8601(readElementText());
                    else if (n == "t0")
                        afr.t0 = readElementText().toDouble();
                    else if (n == "azimuthFmRatePolynomial")
                        afr.polynomial = parsePolynomial(readElementText());
                }
            }
            result.append(afr);
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════
//  第3层：辅助加载
// ═══════════════════════════════════════════════════════════

ProcessingConsistency SlcAnnotationReader::readProcessingConsistency()
{
    ProcessingConsistency pc;
    if (!mXml) return pc;

    reset();
    if (!seekToElement("swathProcParams")) {
        // 可能嵌套在 processingInformation 中
        reset();
        if (!seekToElement("processingInformation")) return pc;
        if (!seekToElement("swathProcParams")) return pc;
    }

    while (!(mXml->isEndElement() && mXml->name() == "swathProcParams")) {
        mXml->readNext();
        if (mXml->isStartElement()) {
            QStringRef n = mXml->name();
            if (n == "rangeProcessingBandwidth")
                pc.rangeBandwidth = readElementText().toDouble();
            else if (n == "azimuthProcessingBandwidth")
                pc.azimuthBandwidth = readElementText().toDouble();
            else if (n == "windowType")
                pc.windowType = readElementText();
            else if (n == "windowCoefficient")
                pc.windowCoefficient = readElementText().toDouble();
            else if (n == "numberOfLooks")
                pc.numberOfLooks = readElementText().toInt();
        }
    }

    // orbitSource / attitudeSource
    reset();
    if (seekToElement("orbitSource"))
        pc.orbitSource = readElementText();
    reset();
    if (seekToElement("attitudeSource"))
        pc.attitudeSource = readElementText();
    reset();
    pc.srgrApplied = seekToElement("srgrConversionApplied") && readElementText() == "true";
    reset();
    pc.thermalNoiseCorrection = seekToElement("thermalNoiseCorrectionPerformed")
        && readElementText() == "true";

    return pc;
}

void SlcAnnotationReader::readEllipsoidParams(double& semiMajor, double& semiMinor)
{
    semiMajor = 0.0; semiMinor = 0.0;
    if (!mXml) return;

    reset();
    if (seekToElement("ellipsoidSemiMajorAxis"))
        semiMajor = readElementText().toDouble();
    reset();
    if (seekToElement("ellipsoidSemiMinorAxis"))
        semiMinor = readElementText().toDouble();
}

QVector<AttitudeData> SlcAnnotationReader::readAttitudeList()
{
    reset();
    return readAttitudeListNoReset();
}

QVector<AttitudeData> SlcAnnotationReader::readAttitudeListNoReset()
{
    QVector<AttitudeData> result;
    if (!mXml) return result;
    if (!seekToElement("attitudeList")) return result;

    while (!(mXml->isEndElement() && mXml->name() == "attitudeList")) {
        mXml->readNext();
        if (mXml->isStartElement() && mXml->name() == "attitude") {
            AttitudeData ad;
            while (!(mXml->isEndElement() && mXml->name() == "attitude")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    QString v = readElementText();
                    if (n == "time")       ad.time = parseIso8601(v);
                    else if (n == "frame")   ad.frame = v;
                    else if (n == "q0")      ad.q0 = v.toDouble();
                    else if (n == "q1")      ad.q1 = v.toDouble();
                    else if (n == "q2")      ad.q2 = v.toDouble();
                    else if (n == "q3")      ad.q3 = v.toDouble();
                    else if (n == "wx")      ad.wx = v.toDouble();
                    else if (n == "wy")      ad.wy = v.toDouble();
                    else if (n == "wz")      ad.wz = v.toDouble();
                    else if (n == "roll")    ad.roll = v.toDouble();
                    else if (n == "pitch")   ad.pitch = v.toDouble();
                    else if (n == "yaw")     ad.yaw = v.toDouble();
                }
            }
            result.append(ad);
        }
    }
    return result;
}

QualityInfo SlcAnnotationReader::readQualityInfo()
{
    QualityInfo qi;
    if (!mXml) return qi;

    reset();
    if (seekToElement("productQualityIndex"))
        qi.productQualityIndex = readElementText().toDouble();

    // downlinkQuality flags
    reset();
    qi.inputDataMeanOutsideNominalRange = seekToElement("inputDataMeanOutsideNominalRangeFlag")
        && readElementText() == "true";
    reset();
    qi.inputDataStDevOutsideNominalRange = seekToElement("inputDataStDevOutsideNominalRangeFlag")
        && readElementText() == "true";
    reset();
    qi.downlinkGapsSignificant = seekToElement("downlinkGapsInInputDataSignificantFlag")
        && readElementText() == "true";
    reset();
    qi.downlinkMissingSignificant = seekToElement("downlinkMissingLinesSignificantFlag")
        && readElementText() == "true";
    reset();
    qi.instrumentGapsSignificant = seekToElement("instrumentGapsInInputDataSignificantFlag")
        && readElementText() == "true";
    reset();
    qi.instrumentMissingSignificant = seekToElement("instrumentMissingLinesSignificantFlag")
        && readElementText() == "true";

    // doppler quality
    reset();
    qi.dopplerCentroidUncertain = seekToElement("dopplerCentroidUncertainFlag")
        && readElementText() == "true";

    // rawDataAnalysis quality
    reset();
    qi.iBiasSignificant = seekToElement("iBiasSignificanceFlag")
        && readElementText() == "true";
    reset();
    qi.qBiasSignificant = seekToElement("qBiasSignificanceFlag")
        && readElementText() == "true";
    reset();
    qi.iqGainSignificant = seekToElement("iqGainSignificanceFlag")
        && readElementText() == "true";
    reset();
    qi.iqQuadratureSignificant = seekToElement("iqQuadratureDepartureSignificanceFlag")
        && readElementText() == "true";

    return qi;
}

ImageStatistics SlcAnnotationReader::readImageStatistics()
{
    ImageStatistics s;
    if (!mXml) return s;

    reset();
    if (!seekToElement("imageStatistics")) return s;

    while (!(mXml->isEndElement() && mXml->name() == "imageStatistics")) {
        mXml->readNext();
        if (mXml->isStartElement()) {
            QStringRef n = mXml->name();
            if (n == "outputDataMean") {
                while (!(mXml->isEndElement() && mXml->name() == "outputDataMean")) {
                    mXml->readNext();
                    if (mXml->isStartElement()) {
                        if (mXml->name() == "re") s.outputDataMeanRe = readElementText().toDouble();
                        else if (mXml->name() == "im") s.outputDataMeanIm = readElementText().toDouble();
                    }
                }
            } else if (n == "outputDataStdDev") {
                while (!(mXml->isEndElement() && mXml->name() == "outputDataStdDev")) {
                    mXml->readNext();
                    if (mXml->isStartElement()) {
                        if (mXml->name() == "re") s.outputDataStdDevRe = readElementText().toDouble();
                        else if (mXml->name() == "im") s.outputDataStdDevIm = readElementText().toDouble();
                    }
                }
            }
        }
    }

    // 读取 outlier flag (imageStatistics 的兄弟元素)
    reset();
    s.outputDataMeanOutsideNominalRangeFlag =
        seekToElement("outputDataMeanOutsideNominalRangeFlag")
        && readElementText() == "true";

    return s;
}

QString SlcAnnotationReader::readPixelValue()
{
    if (!mXml) return QString();
    reset();
    if (seekToElement("pixelValue")) return readElementText();
    return QString();
}

QString SlcAnnotationReader::readOutputPixels()
{
    if (!mXml) return QString();
    reset();
    if (seekToElement("outputPixels")) return readElementText();
    return QString();
}

double SlcAnnotationReader::readAzimuthTimeInterval()
{
    if (!mXml) return 0.0;
    reset();
    if (seekToElement("azimuthTimeInterval")) return readElementText().toDouble();
    return 0.0;
}

// ═══════════════════════════════════════════════════════════
//  一次性全读
// ═══════════════════════════════════════════════════════════

SlcAnnotation SlcAnnotationReader::readAll()
{
    SlcAnnotation ann;
    if (!mXml) return ann;

    qDebug() << "[SlcReader] readAll start";
    reset(); // 仅此一次

    // ═══ 顺序1: adsHeader → identity ═══
    qDebug() << "[SlcReader]   section 1: adsHeader";
    if (seekToElement(QStringLiteral("adsHeader"))) {
        while (!(mXml->isEndElement() && mXml->name() == "adsHeader")) {
            mXml->readNext();
            if (mXml->isStartElement()) {
                QStringRef n = mXml->name();
                QString v = readElementText();
                if (n == "missionId")            ann.identity.missionId = v;
                else if (n == "productType")      ann.identity.productType = v;
                else if (n == "polarisation")     ann.identity.polarization = v;
                else if (n == "mode")             ann.identity.mode = v;
                else if (n == "swath")            ann.identity.swath = v;
                else if (n == "startTime")        ann.identity.startTime = parseIso8601(v);
                else if (n == "stopTime")         ann.identity.stopTime  = parseIso8601(v);
                else if (n == "absoluteOrbitNumber") ann.identity.absoluteOrbitNumber = v.toInt();
                else if (n == "missionDataTakeId")   ann.identity.missionDataTakeId = v;
            }
        }
    }

    // ═══ 顺序2: qualityInformation → quality ═══
    qDebug() << "[SlcReader]   section 2: qualityInformation";
    if (seekToElement(QStringLiteral("qualityInformation"))) {
        while (!(mXml->isEndElement() && mXml->name() == "qualityInformation")) {
            mXml->readNext();
            if (mXml->isStartElement()) {
                QStringRef n = mXml->name();
                if (n == "productQualityIndex")
                    ann.quality.productQualityIndex = readElementText().toDouble();
                else if (n == "inputDataMeanOutsideNominalRangeFlag")
                    ann.quality.inputDataMeanOutsideNominalRange = (readElementText() == "true");
                else if (n == "inputDataStDevOutsideNominalRangeFlag")
                    ann.quality.inputDataStDevOutsideNominalRange = (readElementText() == "true");
                else if (n == "downlinkGapsInInputDataSignificantFlag")
                    ann.quality.downlinkGapsSignificant = (readElementText() == "true");
                else if (n == "downlinkMissingLinesSignificantFlag")
                    ann.quality.downlinkMissingSignificant = (readElementText() == "true");
                else if (n == "instrumentGapsInInputDataSignificantFlag")
                    ann.quality.instrumentGapsSignificant = (readElementText() == "true");
                else if (n == "instrumentMissingLinesSignificantFlag")
                    ann.quality.instrumentMissingSignificant = (readElementText() == "true");
                else if (n == "dopplerCentroidUncertainFlag")
                    ann.quality.dopplerCentroidUncertain = (readElementText() == "true");
                else if (n == "iBiasSignificanceFlag")
                    ann.quality.iBiasSignificant = (readElementText() == "true");
                else if (n == "qBiasSignificanceFlag")
                    ann.quality.qBiasSignificant = (readElementText() == "true");
                else if (n == "iqGainSignificanceFlag")
                    ann.quality.iqGainSignificant = (readElementText() == "true");
                else if (n == "iqQuadratureDepartureSignificanceFlag")
                    ann.quality.iqQuadratureSignificant = (readElementText() == "true");
            }
        }
    }

    qDebug() << "[SlcReader]   section 3: generalAnnotation";
    // ═══ 顺序3: generalAnnotation → pass, sensor params, orbits, attitude, FM rates ═══
    if (seekToElement(QStringLiteral("generalAnnotation"))) {
        qDebug() << "[SlcReader]     genAnn found";
        // 3a: productInformation → pass, radarFrequency, rangeSamplingRate, azimuthSteeringRate
        if (seekToElement(QStringLiteral("productInformation"))) {
            while (!(mXml->isEndElement() && mXml->name() == "productInformation")) {
                mXml->readNext();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    QString v = readElementText();
                    if (n == "pass") {
                        ann.identity.passDirection = v;
                    } else if (n == "radarFrequency") {
                        ann.radarFrequency = v.toDouble();
                    } else if (n == "rangeSamplingRate") {
                        ann.rangeSamplingRate = v.toDouble();
                    } else if (n == "azimuthSteeringRate") {
                        ann.azimuthSteeringRate = v.toDouble();
                    }
                }
            }
        }
        // 3b: orbitList
        ann.orbitList = readOrbitVectorsNoReset();
        // 3c: attitudeList
        ann.attitudeList = readAttitudeListNoReset();
        // 3d: azimuthFmRateList
        ann.azimuthFmRates = readAzimuthFmRatesNoReset();
    }

    qDebug() << "[SlcReader]   section 4: imageAnnotation";
    // ═══ 顺序4: imageAnnotation → image params + processing + ellipsoid + stats ═══
    if (seekToElement(QStringLiteral("imageAnnotation"))) {
        // 4a: imageInformation
        qDebug() << "[SlcReader]     4a: seeking imageInformation...";
        if (seekToElement(QStringLiteral("imageInformation"))) {
            qDebug() << "[SlcReader]     4a: imageInformation found, entering loop";
            int loopCount = 0;
            while (!(mXml->isEndElement() && mXml->name() == "imageInformation") && !mXml->hasError()) {
                mXml->readNext();
                if (++loopCount % 500 == 0)
                    qDebug() << "[SlcReader]     4a: loopCount=" << loopCount << "line=" << mXml->lineNumber();
                if (mXml->isStartElement()) {
                    QStringRef n = mXml->name();
                    if (n == "slantRangeTime")           ann.slantRangeTime = readElementText().toDouble();
                    else if (n == "pixelValue")           ann.pixelValue = readElementText();
                    else if (n == "outputPixels")         ann.outputPixels = readElementText();
                    else if (n == "azimuthTimeInterval")  ann.azimuthTimeInterval = readElementText().toDouble();
                    else if (n == "azimuthFrequency")     ann.azimuthFrequency = readElementText().toDouble();
                    else if (n == "rangePixelSpacing")    ann.rangePixelSpacing = readElementText().toDouble();
                    else if (n == "azimuthPixelSpacing")  ann.azimuthPixelSpacing = readElementText().toDouble();
                    else if (n == "incidenceAngleMidSwath") ann.incidenceAngleMidSwath = readElementText().toDouble();
                    else if (n == "numberOfSamples")      ann.numberOfSamples = readElementText().toInt();
                    else if (n == "numberOfLines")        ann.numberOfLines = readElementText().toInt();
                    else if (n == "zeroDopMinusAcqTime")  ann.zeroDopMinusAcqTime = readElementText().toDouble();
                    else if (n == "imageStatistics") {
                        qDebug() << "[SlcReader]     4a: imageStatistics found";
                        while (!(mXml->isEndElement() && mXml->name() == "imageStatistics")) {
                            mXml->readNext();
                            if (mXml->isStartElement()) {
                                QStringRef sn = mXml->name();
                                if (sn == "outputDataMean") {
                                    while (!(mXml->isEndElement() && mXml->name() == "outputDataMean")) {
                                        mXml->readNext();
                                        if (mXml->isStartElement()) {
                                            if (mXml->name() == "re") ann.stats.outputDataMeanRe = readElementText().toDouble();
                                            else if (mXml->name() == "im") ann.stats.outputDataMeanIm = readElementText().toDouble();
                                        }
                                    }
                                } else if (sn == "outputDataStdDev") {
                                    while (!(mXml->isEndElement() && mXml->name() == "outputDataStdDev")) {
                                        mXml->readNext();
                                        if (mXml->isStartElement()) {
                                            if (mXml->name() == "re") ann.stats.outputDataStdDevRe = readElementText().toDouble();
                                            else if (mXml->name() == "im") ann.stats.outputDataStdDevIm = readElementText().toDouble();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            qDebug() << "[SlcReader]     4a: imageInformation done, loopCount=" << loopCount;
        }
        // 4b: processingInformation — 跳过嵌套元素解析, 直接用 skipSubtree
        //     内部 swathProcParamsList/rangeProcessing/azimuthProcessing 等容器元素
        //     与 QXmlStreamReader 的 seekToElement 模式不兼容
        qDebug() << "[SlcReader]     4b: skipping processingInformation subtree...";
        if (seekToElement(QStringLiteral("processingInformation"))) {
            skipSubtree();  // 跳到 </processingInformation> 之后
        }
    }

    qDebug() << "[SlcReader]   section 5: dopplerCentroid";
    ann.dopplerEstimates = readDopplerCentroidNoReset();
    qDebug() << "[SlcReader]   doppler done, estimates=" << ann.dopplerEstimates.size()
             << "reader atEnd=" << mXml->atEnd() << "hasError=" << mXml->hasError();

    // ═══ 顺序6: swathTiming → linesPerBurst, samplesPerBurst, burstList ═══
    qDebug() << "[SlcReader]   section 6: swathTiming";
    bool foundSwath = seekToElement(QStringLiteral("swathTiming"));
    qDebug() << "[SlcReader]     seekToElement(swathTiming) =" << foundSwath
             << "atEnd=" << mXml->atEnd() << "hasError=" << mXml->hasError();
    if (foundSwath) {
        while (!(mXml->isEndElement() && mXml->name() == "swathTiming") && !mXml->atEnd()) {
            mXml->readNext();
            if (mXml->isStartElement()) {
                QStringRef n = mXml->name();
                if (n == "linesPerBurst") {
                    ann.linesPerBurst = readElementText().toInt();
                } else if (n == "samplesPerBurst") {
                    ann.samplesPerBurst = readElementText().toInt();
                } else if (n == "burstList") {
                    ann.burstList = readBurstTimingNoReset();
                }
            }
        }
    }

    // ═══ 顺序7: geolocationGrid → geolocationGridPointList ═══
    qDebug() << "[SlcReader]   section 7: geolocationGrid";
    ann.geolocationGrid = readGeolocationGridNoReset();
    qDebug() << "[SlcReader]     grid points=" << ann.geolocationGrid.size();

    qDebug() << "[SlcReader]   readAll done, swath=" << ann.identity.swath
             << "bursts=" << ann.burstList.size() << "grid=" << ann.geolocationGrid.size();
    return ann;
}
