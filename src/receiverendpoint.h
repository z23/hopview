#pragma once

#include <QHostAddress>
#include <QRegularExpression>
#include <QString>

namespace telemetry {
struct ReceiverEndpoint {
    QString host;
    quint16 port = 8347;
};

inline bool parseReceiverEndpoint(const QString &text, ReceiverEndpoint &out) {
    if (text.isEmpty() || text.size() > 320 || text != text.trimmed())
        return false;
    QString host = text, portText;
    const bool bracketed = text.startsWith('[');
    if (bracketed) {
        const auto end = text.indexOf(']');
        if (end < 2)
            return false;
        host = text.mid(1, end - 1);
        if (end + 1 < text.size()) {
            if (text[end + 1] != ':' || end + 2 == text.size())
                return false;
            portText = text.mid(end + 2);
        }
    } else if (text.count(':') == 1) {
        const auto colon = text.indexOf(':');
        host = text.left(colon);
        portText = text.mid(colon + 1);
        if (portText.isEmpty())
            return false;
    }

    const QHostAddress ip(host);
    if (ip.isNull()) {
        static const QRegularExpression dns(QStringLiteral(
            R"(\A(?:[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)*[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.?\z)"));
        if (bracketed || host.size() > 253 || !dns.match(host).hasMatch())
            return false;
    } else {
        static const QRegularExpression literal(QStringLiteral(R"(\A[0-9A-Fa-f:.]+(?:%[a-zA-Z0-9_.-]+)?\z)"));
        if ((bracketed && ip.protocol() != QAbstractSocket::IPv6Protocol) || !literal.match(host).hasMatch())
            return false;
    }
    quint16 port = 8347;
    if (!portText.isEmpty()) {
        static const QRegularExpression digits(QStringLiteral(R"(\A[0-9]{1,5}\z)"));
        bool ok = false;
        port = portText.toUShort(&ok);
        if (!ok || !port || !digits.match(portText).hasMatch())
            return false;
    }
    out = {host, port};
    return true;
}
}
