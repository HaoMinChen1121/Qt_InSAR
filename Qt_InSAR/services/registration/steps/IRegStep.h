#ifndef IREGSTEP_H
#define IREGSTEP_H

#include <QString>

struct PipelineContext;

class IRegStep {
public:
    virtual ~IRegStep() = default;
    virtual bool execute(PipelineContext& ctx) = 0;
    virtual void cancel() { mCancelled = true; }
    virtual QString name() const = 0;
protected:
    bool mCancelled = false;
};

#endif // IREGSTEP_H
