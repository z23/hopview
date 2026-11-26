#pragma once

#include "probe.h"
#include "probeoptions.h"

class ProbeSession : public QObject {
public:
    using QObject::QObject;
    virtual void cancel() = 0;
    virtual void finish() = 0;
    virtual void resolve(QString target, int family) = 0;
};

ProbeSession *newProbeSession(Probe *owner, qint64 id, int intervalMs, ProbeOptions options);
