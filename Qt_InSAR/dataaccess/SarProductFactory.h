#ifndef SARPRODUCTFACTORY_H
#define SARPRODUCTFACTORY_H

#include "ISarProduct.h"
#include "impl/Sentinel1Product.h"
#include <QFileInfo>
#include <QDir>

/**
 * @brief SAR 产品工厂 — 根据文件路径自动识别传感器和产品类型
 *
 * 类比 RS 项目的 SensorProductFactory，通过文件扩展名、
 * 目录结构和文件名模式自动检测传感器类型并创建对应产品对象。
 * 调用方负责 delete 返回的指针。
 */
inline ISarProduct* createSarProduct(const QString& path) {
    QFileInfo fi(path);
    QString base = fi.completeBaseName();

    // ── Sentinel-1 ZIP: S1A_IW_GRDH_...zip 或 S1A_IW_SLC_...zip ──
    if (path.endsWith(".zip", Qt::CaseInsensitive)) {
        if (base.startsWith("S1", Qt::CaseInsensitive)) {
            // 过滤空段 (SLC 文件名有双下划线: S1A_IW_SLC__1SDV_...)
            QStringList parts;
            for (const QString& p : base.split('_')) {
                if (!p.isEmpty()) parts.append(p);
            }
            if (parts.size() >= 3) {
                QString pt = parts[2].toUpper();
                if (pt == "SLC" || pt.startsWith("GRD") || pt == "RAW")
                    return new Sentinel1Product();
            }
        }
    }

    // ── .SAFE 目录 (解压后的) ──
    if (fi.isDir()) {
        if (fi.fileName().endsWith(".SAFE", Qt::CaseInsensitive)
            && QFileInfo::exists(path + "/manifest.safe"))
            return new Sentinel1Product();
        if (QFileInfo::exists(path + "/manifest.safe"))
            return new Sentinel1Product();

        // 高分三号 — 预留
        QString dn = fi.fileName();
        if (dn.contains("GF-3", Qt::CaseInsensitive) ||
            dn.contains("GF3", Qt::CaseInsensitive))
            return nullptr;
    }

    // ── manifest.safe 文件 ──
    if (fi.fileName() == "manifest.safe")
        return new Sentinel1Product();

    return nullptr;
}

#endif // SARPRODUCTFACTORY_H
