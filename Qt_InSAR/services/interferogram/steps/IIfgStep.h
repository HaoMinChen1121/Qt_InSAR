#ifndef IFG_ISTEP_H
#define IFG_ISTEP_H

#include <QString>

struct IfgPipelineContext;

class IIfgStep {
public:
    virtual ~IIfgStep() = default;
    virtual bool execute(IfgPipelineContext& ctx) = 0;
    virtual void cancel() { mCancelled = true; }
    virtual QString name() const = 0;
protected:
    bool mCancelled = false;
};

#endif // IFG_ISTEP_H
