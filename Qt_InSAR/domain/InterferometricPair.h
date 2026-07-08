#ifndef INTERFEROMETRICPAIR_H
#define INTERFEROMETRICPAIR_H

#include <QString>
#include "BaselineInfo.h"
#include "SlcImage.h"

struct InterferometricPair
{
    QString       pairId;        // 干涉对ID
    SlcImage      master;        // 主影像
    SlcImage      slave;         // 辅影像
    BaselineInfo  baseline;      // 基线信息
    bool          isRegistered = false; // 是否已完成配准
};

#endif // INTERFEROMETRICPAIR_H
