#pragma once

#include "probeoptions.h"
#include "types.h"
#include <QVariantMap>
#include <functional>
#include <memory>

struct EchoReply {
    QString address;
    double rttMs = -1;
    bool destination = false;
    ProbeFailure failure;
};

// One transport per destination, owned and used on the probe thread.
class NativeEcho {
public:
    using Reply = std::function<void(quint64, EchoReply)>;
    virtual ~NativeEcho() = default;
    virtual void send(quint64 token, int ttl) = 0;
    virtual void discard(quint64 token) = 0;
    virtual QVariantMap route() const = 0;
};

std::unique_ptr<NativeEcho> openNativeEcho(const QHostAddress &address, const ProbeOptions &options,
                                          NativeEcho::Reply reply, QString &error);
