#ifndef TOPSARDERAMP_H
#define TOPSARDERAMP_H

#include "IRegStep.h"

class TOPSARDeramp : public IRegStep {
public:
    bool execute(PipelineContext& ctx) override;
    QString name() const override { return QStringLiteral("TOPSAR Deramp"); }
};

#endif // TOPSARDERAMP_H
