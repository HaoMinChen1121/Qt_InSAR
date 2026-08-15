#ifndef FILTERUNWRAPPARAMS_H
#define FILTERUNWRAPPARAMS_H

#include "domain/params/FilterParams.h"
#include "domain/params/UnwrappingParams.h"

// 滤波+解缠合并流程参数 (Ribbon 单一执行入口)
// inputProductId: 干涉产品 qsar 路径 — 经 ProductManager 解析, 不传文件路径
struct FilterUnwrapParams
{
    QString         inputProductId;
    FilterParams    filter;
    UnwrappingParams unwrap;
};

#endif // FILTERUNWRAPPARAMS_H
