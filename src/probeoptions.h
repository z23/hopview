#pragma once

#include <QString>
#include <QHostAddress>
#include <QJsonObject>
#include <cmath>

struct ProbeOptions {
    QString protocol = QStringLiteral("auto");
    int port = 0;
    QString interfaceName;

    int payloadBytes = 16;
    int dscp = 0;
    bool dontFragment = false;

    bool valid() const {
        return (protocol == "auto" || protocol == "icmp" || protocol == "udp" || protocol == "tcp")
            && payloadBytes >= 8 && payloadBytes <= 65507 && dscp >= 0 && dscp <= 63
            && (protocol != "tcp" || (payloadBytes == 16 && !dontFragment))
            && port >= 0 && port <= 65535 && (protocol != "icmp" || !port);
    }

    int destinationPort() const {
        if (protocol == "icmp") return 0;
        if (port) return port;
        return protocol == "tcp" ? 443 : 33434;
    }
};

// Receiver assignments and portable recordings may contain profiles from any OS.
// Apply these limits only when starting a probe on this machine.
inline QString localProbeError(const ProbeOptions &options, int family) {
    if (!options.valid())
        return QStringLiteral("Check the protocol, port, payload size and DSCP. TCP uses the system SYN size.");
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    if (options.protocol != "auto" && options.protocol != "icmp")
        return QStringLiteral("Local traces on this platform support Auto or ICMP.");
    if (family == 6 && options.dontFragment)
        return QStringLiteral("Unfragmented probes on this platform require IPv4.");
#ifdef Q_OS_WIN
    if (options.dscp)
        return QStringLiteral("The Windows ICMP API cannot guarantee the requested DSCP marking.");
#endif
#else
    Q_UNUSED(family);
#endif
    return {};
}

inline bool validMeasurement(const QJsonObject &m) {
    const auto family = m["effectiveFamily"].toInt(), port = m["effectivePort"].toInt(-1);
    const auto method = m["effectiveProtocol"].toString();
    const auto address = QHostAddress(m["resolvedAddress"].toString());
    if ((family != 4 && family != 6) || address.isNull() ||
        (family == 4) != (address.protocol() == QAbstractSocket::IPv4Protocol) ||
        !ProbeOptions{method,port,{}}.valid() || method == "auto") return false;
    for (auto it = m.begin(); it != m.end(); ++it) {
        const auto k = it.key();
        if (k == "effectiveFamily" || k == "effectiveProtocol" || k == "effectivePort" || k == "resolvedAddress") continue;
        if (k == "clockOffsetMs" || k == "clockErrorMs") {
            if (!it.value().isDouble() || std::abs(it.value().toDouble()) > 253402300799999.) return false;
        } else if (k == "effectiveInterface" || k == "sourceAddress" || k == "flowMode") {
            if (!it.value().isString() || it.value().toString().size() > 255) return false;
        } else return false;
    }
    return true;
}
