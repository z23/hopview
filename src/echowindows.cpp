#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <netioapi.h>

#include "nativeecho.h"
#include <QNetworkInterface>
#include <QObject>
#include <QSet>
#include <cstring>
#include <mutex>

namespace {
NET_IFINDEX interfaceIndex(const QString &name) {
    bool ok = false;
    const auto numeric = name.toUInt(&ok);
    return ok ? numeric : QNetworkInterface::interfaceIndexFromName(name);
}

SOCKADDR_INET socketAddress(const QHostAddress &ip) {
    SOCKADDR_INET result{};
    if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
        result.Ipv4.sin_family = AF_INET;
        result.Ipv4.sin_addr.s_addr = htonl(ip.toIPv4Address());
    } else {
        result.Ipv6.sin6_family = AF_INET6;
        const auto address = ip.toIPv6Address();
        std::memcpy(&result.Ipv6.sin6_addr, address.c, sizeof(address.c));
        if (!ip.scopeId().isEmpty()) result.Ipv6.sin6_scope_id = interfaceIndex(ip.scopeId());
    }
    return result;
}

QHostAddress ipv6Address(const void *bytes, ULONG scope) {
    QHostAddress address(static_cast<const quint8 *>(bytes));
    if (scope && address.isLinkLocal()) address.setScopeId(QString::number(scope));
    return address;
}

EchoReply resultOf(DWORD status, const QHostAddress &address = {}, double rtt = -1) {
    EchoReply result;
    const QString router = address.isNull() || address.isEqual(QHostAddress::Any) ? QString{} : address.toString();
    switch (status) {
    case IP_SUCCESS:
    case IP_TTL_EXPIRED_TRANSIT:
        if (!router.isEmpty()) {
            result.address = router;
            result.rttMs = rtt;
            result.destination = status == IP_SUCCESS;
        }
        break;
    case IP_REQ_TIMED_OUT:
        break;
    case IP_PACKET_TOO_BIG:
        result.failure = {"packet-too-big", router};
        break;
    case IP_DEST_UNREACHABLE:
    case IP_DEST_NET_UNREACHABLE:
    case IP_DEST_HOST_UNREACHABLE:
    case IP_DEST_PORT_UNREACHABLE:
    case IP_DEST_PROT_UNREACHABLE:
        result.failure = {address.protocol() == QAbstractSocket::IPv6Protocol && status == IP_DEST_PROHIBITED
                            ? "filtered" : "unreachable", router};
        break;
    case IP_PARAM_PROBLEM:
    case IP_TTL_EXPIRED_REASSEM:
    case IP_TIME_EXCEEDED:
    case IP_ICMP_ERROR:
    case IP_SOURCE_QUENCH:
    case IP_UNRECOGNIZED_NEXT_HEADER:
        result.failure = {"icmp-error", router};
        break;
    default:
        // The helper reports Win32/IP_STATUS values, not ICMP type/code or POSIX errno.
        result.rttMs = -2;
        result.failure.kind = "local-unsent";
        break;
    }
    return result;
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) IcmpCloseHandle(value); }
};

class WindowsEcho;
struct Delivery {
    std::mutex mutex;
    WindowsEcho *receiver = nullptr;
};

// Windows owns the reply buffer until its event signals, including after a session stops.
struct Request {
    std::shared_ptr<Delivery> delivery;
    std::shared_ptr<Handle> handle;
    quint64 token = 0;
    bool ipv6 = false;
    HANDLE event = nullptr;
    PTP_WAIT wait = nullptr;
    QByteArray payload, buffer;
    IP_OPTION_INFORMATION options{};
    SOCKADDR_INET source{}, target{};

    ~Request() {
        if (wait) CloseThreadpoolWait(wait);
        if (event) CloseHandle(event);
    }

    EchoReply decoded(DWORD error = NO_ERROR) const {
        if (ipv6) {
            ICMPV6_ECHO_REPLY reply{};
            std::memcpy(&reply, buffer.constData(), sizeof(reply));
            if (error && reply.Status != error) return resultOf(error);
            // IP Helper packs IPV6_ADDRESS_EX at byte alignment: it is a sockaddr_in6
            // without its family. Some MinGW headers leave padding before the flow field.
            sockaddr_in6 address{};
            static_assert(offsetof(ICMPV6_ECHO_REPLY, Status) == 28);
            std::memcpy(&address.sin6_port, buffer.constData(), sizeof(address) - offsetof(sockaddr_in6, sin6_port));
            return resultOf(reply.Status, ipv6Address(&address.sin6_addr, address.sin6_scope_id),
                            reply.RoundTripTime);
        }
        // The fields used here have identical offsets in ICMP_ECHO_REPLY and ICMP_ECHO_REPLY32.
        ICMP_ECHO_REPLY reply{};
        std::memcpy(&reply, buffer.constData(), sizeof(reply));
        if (error && reply.Status != error) return resultOf(error);
        return resultOf(reply.Status, QHostAddress(ntohl(reply.Address)), reply.RoundTripTime);
    }

    DWORD issue() {
        void *data = payload.data(), *out = buffer.data();
        if (ipv6)
            return Icmp6SendEcho2(handle->value, event, nullptr, nullptr, &source.Ipv6, &target.Ipv6,
                                  data, WORD(payload.size()), &options, out, DWORD(buffer.size()), 1500);
        return IcmpSendEcho2Ex(handle->value, event, nullptr, nullptr,
                              source.Ipv4.sin_addr.s_addr, target.Ipv4.sin_addr.s_addr,
                              data, WORD(payload.size()), &options, out, DWORD(buffer.size()), 1500);
    }

    static void CALLBACK completed(PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT, TP_WAIT_RESULT);
};

class WindowsEcho final : public QObject, public NativeEcho {
public:
    WindowsEcho(SOCKADDR_INET target, NET_IFINDEX index, ProbeOptions options, Reply reply)
        : target_(target), index_(index), options_(std::move(options)), reply_(std::move(reply)) {
        delivery_->receiver = this;
        handle_->value = target_.si_family == AF_INET6 ? Icmp6CreateFile() : IcmpCreateFile();
    }

    ~WindowsEcho() override {
        std::lock_guard lock(delivery_->mutex);
        delivery_->receiver = nullptr;
    }

    bool validHandle() const { return handle_->value != INVALID_HANDLE_VALUE; }

    QVariantMap route() const override {
        MIB_IPFORWARD_ROW2 route{};
        SOCKADDR_INET source{};
        if (GetBestRoute2(nullptr, index_, nullptr, &target_, 0, &route, &source) != NO_ERROR) {
            source_ = {};
            return {};
        }
        source_ = source;
        const auto ip = source.si_family == AF_INET ? QHostAddress(ntohl(source.Ipv4.sin_addr.s_addr))
            : ipv6Address(&source.Ipv6.sin6_addr, source.Ipv6.sin6_scope_id);
        return {{"sourceAddress", ip.toString()},
                {"effectiveInterface", QNetworkInterface::interfaceNameFromIndex(route.InterfaceIndex)}};
    }

    void discard(quint64 token) override { pending_.remove(token); }

    void send(quint64 token, int ttl) override {
        if (index_ && !source_.si_family) {
            reply_(token, resultOf(IP_BAD_ROUTE));
            return;
        }
        auto req = std::make_unique<Request>();
        req->delivery = delivery_;
        req->handle = handle_;
        req->token = token;
        req->ipv6 = target_.si_family == AF_INET6;
        req->target = target_;
        if (index_) req->source = source_;
        else req->source.si_family = target_.si_family;
        req->options.Ttl = UCHAR(ttl);
        req->options.Flags = options_.dontFragment ? IP_FLAG_DF : 0;
        req->payload = QByteArray(options_.payloadBytes, '\0');
        // IP Helper needs space for the reply header, payload, ICMP error and IO_STATUS_BLOCK.
        req->buffer = QByteArray(options_.payloadBytes + 128, '\0');
        req->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        req->wait = CreateThreadpoolWait(&Request::completed, req.get(), nullptr);
        if (!req->event || !req->wait) {
            reply_(token, resultOf(IP_NO_RESOURCES));
            return;
        }
        const DWORD count = req->issue();
        const DWORD error = count ? NO_ERROR : GetLastError();
        if (count && count != ERROR_IO_PENDING) {
            reply_(token, req->decoded());
        } else if (count == ERROR_IO_PENDING || error == ERROR_IO_PENDING) {
            pending_.insert(token);
            // Arm last. An already-signalled event can immediately transfer ownership to its callback.
            auto *request = req.release();
            SetThreadpoolWait(request->wait, request->event, nullptr);
        } else {
            reply_(token, req->decoded(error));
        }
    }

    void deliver(quint64 token, EchoReply reply) {
        if (pending_.remove(token)) reply_(token, std::move(reply));
    }
private:
    SOCKADDR_INET target_;
    NET_IFINDEX index_;
    ProbeOptions options_;
    Reply reply_;
    mutable SOCKADDR_INET source_{};
    std::shared_ptr<Handle> handle_ = std::make_shared<Handle>();
    std::shared_ptr<Delivery> delivery_ = std::make_shared<Delivery>();
    QSet<quint64> pending_;
};

void CALLBACK Request::completed(PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT, TP_WAIT_RESULT) {
    std::unique_ptr<Request> request(static_cast<Request *>(context));
    auto *buffer = request->buffer.data();
    const DWORD size = DWORD(request->buffer.size());
    const DWORD count = request->ipv6 ? Icmp6ParseReplies(buffer, size) : IcmpParseReplies(buffer, size);
    const DWORD error = count ? NO_ERROR : GetLastError();
    // A received ICMP error can produce zero echo replies while the buffer
    // still contains the router address and RTT for the matching status.
    auto result = request->decoded(error);
    std::lock_guard lock(request->delivery->mutex);
    if (auto *receiver = request->delivery->receiver) {
        QMetaObject::invokeMethod(receiver, [receiver, token = request->token, result = std::move(result)] {
            receiver->deliver(token, result);
        }, Qt::QueuedConnection);
    }
}
} // namespace

std::unique_ptr<NativeEcho> openNativeEcho(const QHostAddress &address, const ProbeOptions &options,
                                          NativeEcho::Reply reply, QString &error) {
    const auto target = socketAddress(address);
    const auto index = options.interfaceName.isEmpty() ? 0 : interfaceIndex(options.interfaceName);
    if (!options.interfaceName.isEmpty() && !index) {
        error = "Unknown network interface: " + options.interfaceName;
        return {};
    }
    if (!address.scopeId().isEmpty() && !target.Ipv6.sin6_scope_id) {
        error = "Unknown IPv6 scope: " + address.scopeId();
        return {};
    }
    if (target.si_family == AF_INET6 && target.Ipv6.sin6_scope_id && index && target.Ipv6.sin6_scope_id != index) {
        error = "IPv6 scope and selected interface disagree.";
        return {};
    }
    if (options.dscp || (target.si_family == AF_INET6 && options.dontFragment)) {
        error = "Windows ICMP supports IPv4 Don't Fragment and default DSCP.";
        return {};
    }
    auto transport = std::make_unique<WindowsEcho>(target, index, options, std::move(reply));
    if (!transport->validHandle()) {
        error = "Could not open Windows ICMP (system error " + QString::number(GetLastError()) + ").";
        return {};
    }
    if (transport->route().isEmpty()) {
        error = "Windows found no route to " + address.toString() + " on the selected interface.";
        return {};
    }
    return transport;
}
