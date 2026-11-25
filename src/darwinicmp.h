#pragma once

#include <QByteArrayView>
#include <QHostAddress>
#include <QtEndian>
#include <cstring>
#include <limits>
#include <optional>

// Darwin's IPv4 ICMP socket delivers the outer IP header; its IPv6 socket starts at ICMPv6.
namespace DarwinIcmp {
struct Message {
    quint32 key = 0; // Original echo identifier and sequence, in host byte order.
    quint64 cookie = 0; // Echo replies carry our full payload cookie.
    int type = 0, code = 0, mtu = 0;
    bool echo = false, transit = false;
};

inline int ipv4Header(QByteArrayView packet) {
    if (packet.size() < 20 || (quint8(packet[0]) >> 4) != 4 || packet[9] != 1) return 0;
    const int bytes = (quint8(packet[0]) & 15) * 4;
    return bytes >= 20 && bytes <= packet.size() ? bytes : 0;
}

inline int quotedHeader(QByteArrayView packet, const QHostAddress &destination, bool v6) {
    if (!v6) {
        const int header = ipv4Header(packet);
        if (!header || (qFromBigEndian<quint16>(packet.data() + 6) & 0x1fff)) return 0;
        return qFromBigEndian<quint32>(packet.data() + 16) == destination.toIPv4Address() ? header : 0;
    }
    if (packet.size() < 40 || (quint8(packet[0]) >> 4) != 6) return 0;
    const auto ip = destination.toIPv6Address();
    if (std::memcmp(packet.data() + 24, ip.c, sizeof(ip.c)) != 0) return 0;
    int next = quint8(packet[6]), offset = 40;
    while (next != 58) {
        if (packet.size() < offset + 8) return 0;
        const auto *extension = packet.data() + offset;
        if (next == 0 || next == 43 || next == 60) { // Hop-by-hop, routing or destination options.
            offset += (quint8(extension[1]) + 1) * 8;
        } else if (next == 44) {
            if (qFromBigEndian<quint16>(extension + 2) & 0xfff8) return 0;
            offset += 8;
        } else if (next == 51) {
            offset += (quint8(extension[1]) + 2) * 4;
        } else {
            return 0;
        }
        if (offset > packet.size()) return 0;
        next = quint8(extension[0]);
    }
    return offset;
}

inline std::optional<Message> parse(QByteArrayView packet, const QHostAddress &destination) {
    const bool v6 = destination.protocol() == QAbstractSocket::IPv6Protocol;
    if (!v6) {
        const int header = ipv4Header(packet);
        if (!header) return {};
        packet = packet.sliced(header);
    }
    if (packet.size() < 8) return {};
    Message message;
    message.type = quint8(packet[0]);
    message.code = quint8(packet[1]);
    message.echo = message.type == (v6 ? 129 : 0) && message.code == 0;
    if (message.echo) {
        if (packet.size() < 16) return {};
        message.cookie = qFromBigEndian<quint64>(packet.data() + 8);
    } else {
        if (v6 ? (message.type < 1 || message.type > 4)
               : (message.type != 3 && message.type != 11 && message.type != 12)) return {};
        message.transit = message.type == (v6 ? 3 : 11) && message.code == 0;
        if (v6 && message.type == 2) {
            const auto mtu = qFromBigEndian<quint32>(packet.data() + 4);
            if (mtu <= std::numeric_limits<int>::max()) message.mtu = int(mtu);
        }
        if (!v6 && message.type == 3 && message.code == 4)
            message.mtu = qFromBigEndian<quint16>(packet.data() + 6);
        packet = packet.sliced(8);
        const int header = quotedHeader(packet, destination, v6);
        if (!header || packet.size() < header + 8) return {};
        packet = packet.sliced(header);
        if (quint8(packet[0]) != (v6 ? 128 : 8) || packet[1] != 0) return {};
    }
    message.key = qFromBigEndian<quint32>(packet.data() + 4);
    return message;
}

inline quint16 checksum(QByteArrayView packet) {
    quint32 sum = 0;
    qsizetype i = 0;
    for (; i + 1 < packet.size(); i += 2)
        sum += qFromBigEndian<quint16>(packet.data() + i);
    if (i < packet.size()) sum += quint16(quint8(packet[i])) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return quint16(~sum);
}
} // namespace DarwinIcmp
