#pragma once

#include <QString>

struct ProbeOptions {
    QString protocol = QStringLiteral("auto");
    int port = 0;
    QString interfaceName;

    bool valid() const {
        return (protocol == "auto" || protocol == "icmp" || protocol == "udp" || protocol == "tcp")
            && port >= 0 && port <= 65535 && (protocol != "icmp" || !port);
    }

    int destinationPort() const {
        if (protocol == "icmp") return 0;
        if (port) return port;
        return protocol == "tcp" ? 443 : 33434;
    }
};
