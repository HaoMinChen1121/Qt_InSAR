#ifndef PIPELINEBUILDER_H
#define PIPELINEBUILDER_H

#include <QVector>
#include "domain/registration/RegistrationStrategy.h"

class IRegStep;

// 按策略构建配准步骤链
// 阶段4: 所有策略当前返回相同的 11 步 (先建立机制);
//        待 TOPS Standard 验证结果一致后, 再允许不同产品模式拥有不同管线
QVector<IRegStep*> buildPipelineSteps(const RegistrationStrategy& strat);

#endif // PIPELINEBUILDER_H
