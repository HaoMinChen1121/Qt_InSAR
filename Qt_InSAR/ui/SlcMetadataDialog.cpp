#include "SlcMetadataDialog.h"

#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QClipboard>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileDialog>
#include <QFile>
#include <QLabel>
#include <QHeaderView>

const QColor SlcMetadataDialog::kColorRed(200, 50, 50);
const QColor SlcMetadataDialog::kColorOrange(200, 140, 0);
const QColor SlcMetadataDialog::kColorGray(140, 140, 140);

// ── 辅助：格式化 orbit vector ──
static QString fmtOrbit(const OrbitVector& ov) {
    return QStringLiteral("%1  pos=(%2,%3,%4)  vel=(%5,%6,%7)")
        .arg(ov.utcTime.toString("yyyy-MM-ddTHH:mm:ss.zzz"))
        .arg(ov.posX, 0, 'f', 2).arg(ov.posY, 0, 'f', 2).arg(ov.posZ, 0, 'f', 2)
        .arg(ov.velX, 0, 'f', 3).arg(ov.velY, 0, 'f', 3).arg(ov.velZ, 0, 'f', 3);
}

static QString fmtGeo(const GeolocationPoint& gp) {
    return QStringLiteral("line=%1 pixel=%2  lat=%3° lon=%4°  h=%5m  inc=%6°")
        .arg(gp.line).arg(gp.pixel)
        .arg(gp.latitude, 0, 'f', 4).arg(gp.longitude, 0, 'f', 4)
        .arg(gp.height, 0, 'f', 2).arg(gp.incidenceAngle, 0, 'f', 2);
}

// ═══════════════════════════════════════════════════════════
//  构造 / 布局
// ═══════════════════════════════════════════════════════════

SlcMetadataDialog::SlcMetadataDialog(const QMap<QString, ProductSourceInfo>& allProducts,
                                     const QString& initialKey, QWidget* parent)
    : QDialog(parent), mAllProducts(allProducts), mCurrentKey(initialKey)
{
    setWindowTitle(QStringLiteral("SLC Annotation 元数据"));
    resize(1900, 1440);
    setMinimumSize(1050, 680);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 4, 6, 6);
    mainLayout->setSpacing(4);

    // 顶部栏: 产品选择 + 波段选择 (包裹在 QWidget 中限制高度)
    auto* topWidget = new QWidget;
    topWidget->setMaximumHeight(34);
    auto* topBar = new QHBoxLayout(topWidget);
    topBar->setContentsMargins(0, 0, 0, 0);
    topBar->setSpacing(6);

    auto* lblProd = new QLabel(QStringLiteral("产品:"));
    lblProd->setFixedHeight(24);
    lblProd->setAlignment(Qt::AlignVCenter);
    topBar->addWidget(lblProd);

    mProductCombo = new QComboBox;
    mProductCombo->setMinimumWidth(300);
    mProductCombo->setMaximumHeight(26);
    for (auto it = mAllProducts.constBegin(); it != mAllProducts.constEnd(); ++it) {
        QStringList bands;
        for (const auto& b : it->bands)
            bands.append(QStringLiteral("%1/%2").arg(b.subSwath, b.polarization));
        mProductCombo->addItem(QStringLiteral("%1  [%2]").arg(it->displayName, bands.join(", ")),
                               it.key());
        if (it.key() == initialKey)
            mProductCombo->setCurrentIndex(mProductCombo->count() - 1);
    }
    connect(mProductCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SlcMetadataDialog::onProductChanged);
    topBar->addWidget(mProductCombo);

    auto* lblBand = new QLabel(QStringLiteral("波段:"));
    lblBand->setFixedHeight(24);
    lblBand->setAlignment(Qt::AlignVCenter);
    topBar->addWidget(lblBand);

    mBandCombo = new QComboBox;
    mBandCombo->setMinimumWidth(140);
    mBandCombo->setMaximumHeight(26);
    connect(mBandCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SlcMetadataDialog::onBandChanged);
    topBar->addWidget(mBandCombo);

    mInfoLabel = new QLabel;
    mInfoLabel->setFixedHeight(24);
    mInfoLabel->setAlignment(Qt::AlignVCenter);
    mInfoLabel->setStyleSheet("font-size: 14px; padding: 0 4px;");
    topBar->addWidget(mInfoLabel);

    topBar->addStretch();
    mainLayout->addWidget(topWidget);

    rebuildBandCombo();
    updateInfoBar();

    // 左右分栏
    auto* splitter = new QSplitter(Qt::Horizontal);

    // 左侧: QTreeWidget
    mTree = new QTreeWidget;
    mTree->setHeaderLabels({QStringLiteral("字段"), QStringLiteral("值")});
    mTree->setColumnWidth(0, 420);
    mTree->setColumnWidth(1, 600);
    mTree->setAlternatingRowColors(true);
    mTree->header()->setStretchLastSection(true);
    splitter->addWidget(mTree);

    // 右侧: 详情面板
    mDetailView = new QPlainTextEdit;
    mDetailView->setReadOnly(true);
    mDetailView->setFont(QFont("Consolas", 10));
    mDetailView->setPlaceholderText(QStringLiteral("选中左侧字段查看详情..."));
    splitter->addWidget(mDetailView);

    splitter->setSizes({800, 480});
    mainLayout->addWidget(splitter);

    // 底部按钮
    auto* btnLayout = new QHBoxLayout;
    auto* copyBtn = new QPushButton(QStringLiteral("复制选中值"));
    auto* copyAllBtn = new QPushButton(QStringLiteral("复制全部"));
    auto* jsonBtn = new QPushButton(QStringLiteral("导出 JSON"));
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"));
    btnLayout->addWidget(copyBtn);
    btnLayout->addWidget(copyAllBtn);
    btnLayout->addWidget(jsonBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    connect(copyBtn, &QPushButton::clicked, this, &SlcMetadataDialog::copySelectedValue);
    connect(copyAllBtn, &QPushButton::clicked, this, &SlcMetadataDialog::copyAll);
    connect(jsonBtn, &QPushButton::clicked, this, &SlcMetadataDialog::exportJson);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    // 选中项 → 右侧详情
    connect(mTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        mDetailView->setPlainText(item ? item->text(1) : QString());
    });

    populateTree();
    mTree->expandToDepth(2);
}

void SlcMetadataDialog::updateInfoBar()
{
    const auto& si = mAllProducts.value(mCurrentKey).sensorInfo;
    const auto& bands = mAllProducts.value(mCurrentKey).bands;
    mInfoLabel->setText(QStringLiteral("%1  |  轨道 %2  |  %3  |  %4 波段")
        .arg(mAllProducts.value(mCurrentKey).displayName)
        .arg(si.relativeOrbit)
        .arg(si.acquisitionStart.toString("yyyy-MM-dd HH:mm"))
        .arg(bands.size()));
}

void SlcMetadataDialog::rebuildBandCombo()
{
    mBandCombo->blockSignals(true);
    mBandCombo->clear();
    const auto& annotations = mAllProducts.value(mCurrentKey).annotationsBySwath;
    if (annotations.isEmpty()) {
        mBandCombo->addItem(QStringLiteral("(无缓存)"));
    } else {
        for (auto it = annotations.constBegin(); it != annotations.constEnd(); ++it) {
            mBandCombo->addItem(it.key(), it.key());
        }
        // 默认选第一个
        if (!mCurrentBand.isEmpty() && annotations.contains(mCurrentBand))
            mBandCombo->setCurrentText(mCurrentBand);
        mCurrentBand = mBandCombo->currentData().toString();
        mAnn = annotations.value(mCurrentBand);
    }
    mBandCombo->blockSignals(false);
}

void SlcMetadataDialog::onProductChanged(int index)
{
    QString key = mProductCombo->itemData(index).toString();
    if (key.isEmpty() || key == mCurrentKey) return;
    mCurrentKey = key;
    mCurrentBand.clear();
    rebuildBandCombo();
    updateInfoBar();
    repopulate();
    mTree->expandToDepth(2);
}

void SlcMetadataDialog::onBandChanged(int index)
{
    QString band = mBandCombo->itemData(index).toString();
    if (band.isEmpty() || band == mCurrentBand) return;
    mCurrentBand = band;
    const auto& annotations = mAllProducts.value(mCurrentKey).annotationsBySwath;
    mAnn = annotations.value(band);
    repopulate();
    mTree->expandToDepth(2);
}

void SlcMetadataDialog::repopulate()
{
    mTree->clear();
    populateTree();
}

// ═══════════════════════════════════════════════════════════
//  树节点辅助
// ═══════════════════════════════════════════════════════════

QTreeWidgetItem* SlcMetadataDialog::addHeader(
    QTreeWidgetItem* parent, const QString& title)
{
    auto* item = parent
        ? new QTreeWidgetItem(parent)
        : new QTreeWidgetItem(mTree);
    item->setText(0, title);
    item->setFont(0, QFont(item->font(0).family(), -1, QFont::Bold));
    return item;
}

QTreeWidgetItem* SlcMetadataDialog::addNode(
    QTreeWidgetItem* parent, const QString& label, const QString& value,
    const QColor& color)
{
    auto* item = parent
        ? new QTreeWidgetItem(parent)
        : new QTreeWidgetItem(mTree);
    item->setText(0, label);
    item->setText(1, value);
    if (color != Qt::black) {
        item->setForeground(0, color);
        item->setForeground(1, color);
    }
    return item;
}

QTreeWidgetItem* SlcMetadataDialog::addBoolNode(
    QTreeWidgetItem* parent, const QString& label, bool value)
{
    return addNode(parent, label, value ? QStringLiteral("true") : QStringLiteral("false"),
                   value ? kColorRed : Qt::black);
}

// ═══════════════════════════════════════════════════════════
//  9 个模块填充
// ═══════════════════════════════════════════════════════════

void SlcMetadataDialog::fillAdsHeader(QTreeWidgetItem* root)
{
    const auto& id = mAnn.identity;
    addNode(root, QStringLiteral("missionId"),            id.missionId);
    addNode(root, QStringLiteral("productType"),          id.productType);
    addNode(root, QStringLiteral("polarization"),         id.polarization);
    addNode(root, QStringLiteral("mode"),                 id.mode);
    addNode(root, QStringLiteral("swath"),                id.swath);
    addNode(root, QStringLiteral("startTime"),            id.startTime.toString(Qt::ISODateWithMs));
    addNode(root, QStringLiteral("stopTime"),             id.stopTime.toString(Qt::ISODateWithMs));
    addNode(root, QStringLiteral("absoluteOrbitNumber"),  QString::number(id.absoluteOrbitNumber));
    addNode(root, QStringLiteral("missionDataTakeId"),    id.missionDataTakeId);
    addNode(root, QStringLiteral("passDirection"),        id.passDirection);
}

void SlcMetadataDialog::fillQuality(QTreeWidgetItem* root)
{
    const auto& q = mAnn.quality;
    addNode(root, QStringLiteral("productQualityIndex"),
        QString::number(q.productQualityIndex, 'f', 1),
        q.productQualityIndex != 0.0 ? kColorRed : Qt::black);
    addBoolNode(root, QStringLiteral("inputDataMeanOutsideNominalRange"),  q.inputDataMeanOutsideNominalRange);
    addBoolNode(root, QStringLiteral("inputDataStDevOutsideNominalRange"), q.inputDataStDevOutsideNominalRange);
    addBoolNode(root, QStringLiteral("downlinkGapsSignificant"),           q.downlinkGapsSignificant);
    addBoolNode(root, QStringLiteral("downlinkMissingSignificant"),        q.downlinkMissingSignificant);
    addBoolNode(root, QStringLiteral("instrumentGapsSignificant"),         q.instrumentGapsSignificant);
    addBoolNode(root, QStringLiteral("instrumentMissingSignificant"),      q.instrumentMissingSignificant);
    addBoolNode(root, QStringLiteral("dopplerCentroidUncertain"),          q.dopplerCentroidUncertain);
    addBoolNode(root, QStringLiteral("iBiasSignificant"),                  q.iBiasSignificant);
    addBoolNode(root, QStringLiteral("qBiasSignificant"),                  q.qBiasSignificant);
    addBoolNode(root, QStringLiteral("iqGainSignificant"),                 q.iqGainSignificant);
    addBoolNode(root, QStringLiteral("iqQuadratureSignificant"),           q.iqQuadratureSignificant);
}

void SlcMetadataDialog::fillGeneralAnnotation(QTreeWidgetItem* root)
{
    // 3.1 productInformation
    auto* prodInfo = addHeader(root, QStringLiteral("3.1 productInformation"));
    addNode(prodInfo, QStringLiteral("radarFrequency"),
        QStringLiteral("%1 Hz").arg(mAnn.radarFrequency, 0, 'e', 6),
        mAnn.radarFrequency == 0.0 ? kColorOrange : Qt::black);
    addNode(prodInfo, QStringLiteral("rangeSamplingRate"),
        QStringLiteral("%1 Hz").arg(mAnn.rangeSamplingRate, 0, 'e', 6),
        mAnn.rangeSamplingRate == 0.0 ? kColorOrange : Qt::black);
    addNode(prodInfo, QStringLiteral("azimuthSteeringRate"),
        QStringLiteral("%1 deg/s").arg(mAnn.azimuthSteeringRate, 0, 'f', 4),
        mAnn.azimuthSteeringRate == 0.0 ? kColorOrange : Qt::black);

    // 3.2 orbitList
    auto* orbitHdr = addHeader(root, QStringLiteral("3.2 orbitList (%1 vectors)").arg(mAnn.orbitList.size()));
    if (mAnn.orbitList.isEmpty()) {
        addNode(orbitHdr, QStringLiteral("(未读取)"), QString(), kColorGray);
    } else {
        for (int i = 0; i < mAnn.orbitList.size(); ++i) {
            addNode(orbitHdr, QStringLiteral("[%1]").arg(i), fmtOrbit(mAnn.orbitList[i]));
        }
    }

    // 3.3 attitudeList
    auto* attHdr = addHeader(root, QStringLiteral("3.3 attitudeList (%1 records)").arg(mAnn.attitudeList.size()));
    if (mAnn.attitudeList.isEmpty()) {
        addNode(attHdr, QStringLiteral("(未读取)"), QString(), kColorGray);
    } else {
        for (int i = 0; i < mAnn.attitudeList.size(); ++i) {
            const auto& a = mAnn.attitudeList[i];
            addNode(attHdr, QStringLiteral("[%1]").arg(i),
                QStringLiteral("%1  roll=%2° pitch=%3° yaw=%4°")
                    .arg(a.time.toString("yyyy-MM-ddTHH:mm:ss.zzz"))
                    .arg(a.roll, 0, 'f', 2).arg(a.pitch, 0, 'f', 2).arg(a.yaw, 0, 'f', 2));
        }
    }

    // 3.4 azimuthFmRateList
    auto* fmHdr = addHeader(root, QStringLiteral("3.4 azimuthFmRateList (%1 groups)").arg(mAnn.azimuthFmRates.size()));
    if (mAnn.azimuthFmRates.isEmpty()) {
        addNode(fmHdr, QStringLiteral("(未读取)"), QString(), kColorGray);
    } else {
        for (int i = 0; i < mAnn.azimuthFmRates.size(); ++i) {
            const auto& fm = mAnn.azimuthFmRates[i];
            QStringList coeffs;
            for (double c : fm.polynomial) coeffs.append(QString::number(c, 'e', 4));
            addNode(fmHdr, QStringLiteral("[%1]").arg(i),
                QStringLiteral("t0=%1  poly=[%2]")
                    .arg(fm.t0, 0, 'e', 4).arg(coeffs.join(", ")));
        }
    }

    // 3.5 未解析模块提示
    addNode(root, QStringLiteral("3.5 terrainHeightList / noiseList / antennaPattern"),
        QStringLiteral("(当前未解析 — 按需扩展)"), kColorGray);
}

void SlcMetadataDialog::fillImageAnnotation(QTreeWidgetItem* root)
{
    // 4.1 imageInformation
    auto* imgInfo = addHeader(root, QStringLiteral("4.1 imageInformation"));
    addNode(imgInfo, QStringLiteral("slantRangeTime"),
        QStringLiteral("%1 s").arg(mAnn.slantRangeTime, 0, 'e', 6),
        mAnn.slantRangeTime == 0.0 ? kColorOrange : Qt::black);
    addNode(imgInfo, QStringLiteral("pixelValue"),         mAnn.pixelValue);
    addNode(imgInfo, QStringLiteral("outputPixels"),       mAnn.outputPixels);
    addNode(imgInfo, QStringLiteral("azimuthTimeInterval"),
        QStringLiteral("%1 s").arg(mAnn.azimuthTimeInterval, 0, 'e', 6));
    addNode(imgInfo, QStringLiteral("azimuthFrequency"),
        QStringLiteral("%1 Hz").arg(mAnn.azimuthFrequency, 0, 'f', 3));
    addNode(imgInfo, QStringLiteral("rangePixelSpacing"),
        QStringLiteral("%1 m").arg(mAnn.rangePixelSpacing, 0, 'f', 4));
    addNode(imgInfo, QStringLiteral("azimuthPixelSpacing"),
        QStringLiteral("%1 m").arg(mAnn.azimuthPixelSpacing, 0, 'f', 4));
    addNode(imgInfo, QStringLiteral("incidenceAngleMidSwath"),
        QStringLiteral("%1°").arg(mAnn.incidenceAngleMidSwath, 0, 'f', 2));
    addNode(imgInfo, QStringLiteral("numberOfSamples"),     QString::number(mAnn.numberOfSamples));
    addNode(imgInfo, QStringLiteral("numberOfLines"),       QString::number(mAnn.numberOfLines));
    addNode(imgInfo, QStringLiteral("zeroDopMinusAcqTime"),
        QStringLiteral("%1 s").arg(mAnn.zeroDopMinusAcqTime, 0, 'f', 3));

    // 4.2 processingInformation
    auto* proc = addHeader(root, QStringLiteral("4.2 processingInformation"));
    const auto& p = mAnn.processing;
    addNode(proc, QStringLiteral("rangeBandwidth"),     QStringLiteral("%1 Hz").arg(p.rangeBandwidth, 0, 'e', 6));
    addNode(proc, QStringLiteral("azimuthBandwidth"),   QStringLiteral("%1 Hz").arg(p.azimuthBandwidth, 0, 'e', 6));
    addNode(proc, QStringLiteral("windowType"),         p.windowType);
    addNode(proc, QStringLiteral("windowCoefficient"),  QString::number(p.windowCoefficient, 'f', 2));
    addNode(proc, QStringLiteral("numberOfLooks"),      QString::number(p.numberOfLooks));
    addNode(proc, QStringLiteral("orbitSource"),        p.orbitSource);
    addNode(proc, QStringLiteral("attitudeSource"),     p.attitudeSource);
    addBoolNode(proc, QStringLiteral("srgrApplied"),             p.srgrApplied);
    addBoolNode(proc, QStringLiteral("thermalNoiseCorrection"),  p.thermalNoiseCorrection);

    // 4.3 ellipsoid
    auto* ell = addHeader(root, QStringLiteral("4.3 ellipsoid"));
    addNode(ell, QStringLiteral("semiMajorAxis"),
        QStringLiteral("%1 m").arg(mAnn.ellipsoidSemiMajor, 0, 'f', 1),
        mAnn.ellipsoidSemiMajor == 0.0 ? kColorOrange : Qt::black);
    addNode(ell, QStringLiteral("semiMinorAxis"),
        QStringLiteral("%1 m").arg(mAnn.ellipsoidSemiMinor, 0, 'f', 1),
        mAnn.ellipsoidSemiMinor == 0.0 ? kColorOrange : Qt::black);

    // 4.4 imageStatistics
    auto* stats = addHeader(root, QStringLiteral("4.4 imageStatistics"));
    addNode(stats, QStringLiteral("outputDataMean"),
        QStringLiteral("(%1, %2)").arg(mAnn.stats.outputDataMeanRe, 0, 'e', 3)
                                  .arg(mAnn.stats.outputDataMeanIm, 0, 'e', 3));
    addNode(stats, QStringLiteral("outputDataStdDev"),
        QStringLiteral("(%1, %2)").arg(mAnn.stats.outputDataStdDevRe, 0, 'f', 2)
                                  .arg(mAnn.stats.outputDataStdDevIm, 0, 'f', 2));
    addBoolNode(stats, QStringLiteral("outlierFlag"), mAnn.stats.outputDataMeanOutsideNominalRangeFlag);
}

void SlcMetadataDialog::fillDoppler(QTreeWidgetItem* root)
{
    const auto& est = mAnn.dopplerEstimates;
    auto* hdr = addHeader(root, QStringLiteral("dcEstimateList (%1 groups)").arg(est.size()));
    if (est.isEmpty()) {
        addNode(hdr, QStringLiteral("(未读取)"), QString(), kColorGray);
        return;
    }
    for (int i = 0; i < est.size(); ++i) {
        const auto& de = est[i];
        auto* grp = addHeader(hdr, QStringLiteral("[%1] %2").arg(i)
            .arg(de.azimuthTime.toString("HH:mm:ss.zzz")));
        addNode(grp, QStringLiteral("t0"), QStringLiteral("%1 s").arg(de.t0, 0, 'e', 6));
        QStringList gPoly;
        for (double c : de.geometryDcPoly) gPoly.append(QString::number(c, 'e', 4));
        addNode(grp, QStringLiteral("geometryDcPoly"), QStringLiteral("[%1]").arg(gPoly.join(", ")));
        QStringList dPoly;
        for (double c : de.dataDcPoly) dPoly.append(QString::number(c, 'e', 4));
        addNode(grp, QStringLiteral("dataDcPoly"), QStringLiteral("[%1]").arg(dPoly.join(", ")));
        addNode(grp, QStringLiteral("dataDcRmsError"), QString::number(de.dataDcRmsError, 'f', 3));
        addBoolNode(grp, QStringLiteral("rmsErrorAboveThreshold"), de.rmsErrorAboveThreshold);
        addNode(grp, QStringLiteral("fineDce points"), QString::number(de.fineDce.size()));
    }
}

void SlcMetadataDialog::fillSwathTiming(QTreeWidgetItem* root)
{
    addNode(root, QStringLiteral("linesPerBurst"),   QString::number(mAnn.linesPerBurst),
        mAnn.linesPerBurst == 0 ? kColorOrange : Qt::black);
    addNode(root, QStringLiteral("samplesPerBurst"), QString::number(mAnn.samplesPerBurst),
        mAnn.samplesPerBurst == 0 ? kColorOrange : Qt::black);
    addNode(root, QStringLiteral("burstCount"),      QString::number(mAnn.burstList.size()));

    for (int i = 0; i < mAnn.burstList.size(); ++i) {
        const auto& bd = mAnn.burstList[i];
        auto* bItem = addHeader(root, QStringLiteral("[%1] %2")
            .arg(i).arg(bd.azimuthTime.toString("HH:mm:ss.zzz")));
        addNode(bItem, QStringLiteral("azimuthAnxTime"), QString::number(bd.azimuthAnxTime, 'f', 6));
        addNode(bItem, QStringLiteral("sensingTime"),    bd.sensingTime.toString(Qt::ISODateWithMs));
        addNode(bItem, QStringLiteral("byteOffset"),     QString::number(bd.byteOffset));
        addNode(bItem, QStringLiteral("burstIdRelative"),QString::number(bd.burstIdRelative));
        addNode(bItem, QStringLiteral("burstIdAbsolute"),QString::number(bd.burstIdAbsolute));
    }
}

void SlcMetadataDialog::fillGeolocationGrid(QTreeWidgetItem* root)
{
    const auto& grid = mAnn.geolocationGrid;
    auto* hdr = addHeader(root, QStringLiteral("geolocationGridPointList (%1 points)").arg(grid.size()));
    if (grid.isEmpty()) {
        addNode(hdr, QStringLiteral("(未读取)"), QString(), kColorGray);
        return;
    }
    // 显示前 10 个 + 统计
    int showN = qMin(10, grid.size());
    for (int i = 0; i < showN; ++i)
        addNode(hdr, QStringLiteral("[%1]").arg(i), fmtGeo(grid[i]));
    if (grid.size() > 10)
        addNode(hdr, QString::fromUtf8("... 共 %1 个点").arg(grid.size()), QString(), kColorGray);
}

// ═══════════════════════════════════════════════════════════
//  populateTree: 组装 9 个顶级节点
// ═══════════════════════════════════════════════════════════

void SlcMetadataDialog::populateTree()
{
    auto* root1 = addHeader(nullptr, QStringLiteral("1. adsHeader (产品标识头)"));
    fillAdsHeader(root1);
    auto* root2 = addHeader(nullptr, QStringLiteral("2. qualityInformation (质量信息)"));
    fillQuality(root2);
    auto* root3 = addHeader(nullptr, QStringLiteral("3. generalAnnotation (通用标注)"));
    fillGeneralAnnotation(root3);
    auto* root4 = addHeader(nullptr, QStringLiteral("4. imageAnnotation (影像参数)"));
    fillImageAnnotation(root4);
    auto* root5 = addHeader(nullptr, QStringLiteral("5. dopplerCentroid (多普勒质心)"));
    fillDoppler(root5);
    auto* root6 = addHeader(nullptr, QStringLiteral("6. antennaPattern (天线方向图)"));
    addNode(root6, QStringLiteral("(当前未存储)"), QString(), kColorGray);
    auto* root7 = addHeader(nullptr, QStringLiteral("7. swathTiming (TOPS Burst 时序)"));
    fillSwathTiming(root7);
    auto* root8 = addHeader(nullptr, QStringLiteral("8. geolocationGrid (地理定位网格)"));
    fillGeolocationGrid(root8);
    auto* root9 = addHeader(nullptr, QStringLiteral("9. swathMerging / coordinateConversion"));
    addNode(root9, QStringLiteral("(SLC 不做子带合并和地距转换 — 正常)"), QString(), kColorGray);
}

// ═══════════════════════════════════════════════════════════
//  按钮
// ═══════════════════════════════════════════════════════════

void SlcMetadataDialog::copySelectedValue()
{
    QTreeWidgetItem* item = mTree->currentItem();
    if (!item) return;
    QApplication::clipboard()->setText(item->text(1));
}

void SlcMetadataDialog::copyAll()
{
    QStringList lines;
    for (int i = 0; i < mTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* tli = mTree->topLevelItem(i);
        lines.append(tli->text(0));
        for (int j = 0; j < tli->childCount(); ++j) {
            QTreeWidgetItem* child = tli->child(j);
            lines.append(QStringLiteral("  %1: %2").arg(child->text(0), child->text(1)));
        }
    }
    QApplication::clipboard()->setText(lines.join("\n"));
}

void SlcMetadataDialog::exportJson()
{
    QJsonObject root;

    // identity
    {
        QJsonObject id;
        id["missionId"]           = mAnn.identity.missionId;
        id["productType"]         = mAnn.identity.productType;
        id["polarization"]        = mAnn.identity.polarization;
        id["mode"]                = mAnn.identity.mode;
        id["swath"]               = mAnn.identity.swath;
        id["startTime"]           = mAnn.identity.startTime.toString(Qt::ISODateWithMs);
        id["stopTime"]            = mAnn.identity.stopTime.toString(Qt::ISODateWithMs);
        id["absoluteOrbitNumber"] = mAnn.identity.absoluteOrbitNumber;
        id["missionDataTakeId"]   = mAnn.identity.missionDataTakeId;
        id["passDirection"]       = mAnn.identity.passDirection;
        root["identity"] = id;
    }

    // sensor params
    {
        QJsonObject sp;
        sp["radarFrequency"]       = mAnn.radarFrequency;
        sp["rangeSamplingRate"]    = mAnn.rangeSamplingRate;
        sp["azimuthSteeringRate"]  = mAnn.azimuthSteeringRate;
        sp["slantRangeTime"]       = mAnn.slantRangeTime;
        sp["rangePixelSpacing"]    = mAnn.rangePixelSpacing;
        sp["azimuthPixelSpacing"]  = mAnn.azimuthPixelSpacing;
        sp["azimuthFrequency"]     = mAnn.azimuthFrequency;
        sp["incidenceAngleMidSwath"] = mAnn.incidenceAngleMidSwath;
        sp["numberOfSamples"]      = mAnn.numberOfSamples;
        sp["numberOfLines"]        = mAnn.numberOfLines;
        sp["linesPerBurst"]        = mAnn.linesPerBurst;
        sp["samplesPerBurst"]      = mAnn.samplesPerBurst;
        root["sensorParams"] = sp;
    }

    // orbitList
    {
        QJsonArray arr;
        for (const auto& ov : mAnn.orbitList) {
            QJsonObject o;
            o["time"] = ov.utcTime.toString(Qt::ISODateWithMs);
            o["posX"] = ov.posX; o["posY"] = ov.posY; o["posZ"] = ov.posZ;
            o["velX"] = ov.velX; o["velY"] = ov.velY; o["velZ"] = ov.velZ;
            arr.append(o);
        }
        root["orbitList"] = arr;
    }

    // burstList
    {
        QJsonArray arr;
        for (const auto& bd : mAnn.burstList) {
            QJsonObject b;
            b["azimuthTime"]    = bd.azimuthTime.toString(Qt::ISODateWithMs);
            b["azimuthAnxTime"] = bd.azimuthAnxTime;
            b["byteOffset"]     = (qint64)bd.byteOffset;
            b["burstIdRelative"] = bd.burstIdRelative;
            b["burstIdAbsolute"] = (qint64)bd.burstIdAbsolute;
            arr.append(b);
        }
        root["burstList"] = arr;
    }

    // quality flags
    {
        QJsonObject q;
        q["productQualityIndex"] = mAnn.quality.productQualityIndex;
        q["downlinkGapsSignificant"]    = mAnn.quality.downlinkGapsSignificant;
        q["dopplerCentroidUncertain"]   = mAnn.quality.dopplerCentroidUncertain;
        root["quality"] = q;
    }

    // processing
    {
        QJsonObject p;
        p["rangeBandwidth"]    = mAnn.processing.rangeBandwidth;
        p["azimuthBandwidth"]  = mAnn.processing.azimuthBandwidth;
        p["windowType"]        = mAnn.processing.windowType;
        p["windowCoefficient"] = mAnn.processing.windowCoefficient;
        p["numberOfLooks"]     = mAnn.processing.numberOfLooks;
        p["orbitSource"]       = mAnn.processing.orbitSource;
        root["processing"] = p;
    }

    QString defaultName = QStringLiteral("%1_%2_%3_annotation.json")
        .arg(mAnn.identity.missionId, mAnn.identity.swath, mAnn.identity.polarization);
    QString path = QFileDialog::getSaveFileName(this,
        QStringLiteral("导出 JSON"), defaultName,
        QStringLiteral("JSON Files (*.json)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}
