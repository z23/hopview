#pragma once

#include <QMetaType>
#include <QJsonObject>
#include <QHostAddress>
#include <QString>
#include <QVector>

struct ProbeFailure {
    QString kind;
    QString router;
    int type = -1, code = -1, mtu = 0, systemError = 0;

    QJsonObject json() const {
        return {{"kind", kind}, {"router", router}, {"type", type}, {"code", code},
                {"mtu", mtu}, {"errno", systemError}};
    }
    static bool read(const QJsonObject &o, ProbeFailure &f) {
        f = {o.value("kind").toString(), o.value("router").toString(), o.value("type").toInt(-2),
             o.value("code").toInt(-2), o.value("mtu").toInt(-1), o.value("errno").toInt(-1)};
        return (f.kind == "local-unsent" || f.kind == "unreachable" || f.kind == "filtered" ||
                f.kind == "packet-too-big" || f.kind == "icmp-error")
            && (f.router.isEmpty() || !QHostAddress(f.router).isNull()) && f.router.size() <= 128
            && f.type >= -1 && f.type <= 255 && f.code >= -1 && f.code <= 255
            && f.mtu >= 0 && f.systemError >= 0 && f.systemError <= 4095;
    }
};

struct Sample {
    qint64 session = 0;
    qint64 timeMs = 0;
    int hop = 0;
    QString address;
    double rttMs = -1;
    bool destination = false;
    ProbeFailure failure{};
};
Q_DECLARE_METATYPE(Sample)
