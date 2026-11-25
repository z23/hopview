#include "nativeecho.h"
#include "darwinicmp.h"

#include <QPointer>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace {
using Clock = std::chrono::steady_clock;

QString systemMessage(int error) {
    return QString::fromLocal8Bit(std::strerror(error));
}

unsigned interfaceIndex(const QString &name) {
    bool numeric = false;
    const auto index = name.toUInt(&numeric);
    return numeric ? index : if_nametoindex(name.toLocal8Bit().constData());
}

QString interfaceName(unsigned index) {
    char name[IF_NAMESIZE]{};
    return index && if_indextoname(index, name) ? QString::fromLocal8Bit(name) : QString{};
}

QHostAddress host(const sockaddr_storage &storage) {
    if (storage.ss_family == AF_INET)
        return QHostAddress(ntohl(reinterpret_cast<const sockaddr_in &>(storage).sin_addr.s_addr));
    if (storage.ss_family != AF_INET6) return {};
    const auto &sa = reinterpret_cast<const sockaddr_in6 &>(storage);
    QHostAddress result(reinterpret_cast<const quint8 *>(&sa.sin6_addr));
    if (result.isLinkLocal()) result.setScopeId(interfaceName(sa.sin6_scope_id));
    return result;
}

class MacEcho final : public NativeEcho {
public:
    MacEcho(QHostAddress address, const ProbeOptions &options, Reply reply)
        : address_(std::move(address)), v6_(address_.protocol() == QAbstractSocket::IPv6Protocol),
          options_(options), reply_(std::move(reply)), packet_(8 + options.payloadBytes, '\0'),
          cookie_(QRandomGenerator::system()->generate64()) {}

    ~MacEcho() override {
        notifier_.reset();
        if (fd_ >= 0) close(fd_);
    }

    bool open(QString &error) {
        if (!options_.interfaceName.isEmpty()) {
            interface_ = interfaceIndex(options_.interfaceName);
            if (interfaceName(interface_).isEmpty()) {
                error = "Unknown network interface: " + options_.interfaceName;
                return false;
            }
        }
        if (v6_ && address_.isLinkLocal()) {
            const auto scope = address_.scopeId().isEmpty() ? interface_ : interfaceIndex(address_.scopeId());
            if (!scope || interfaceName(scope).isEmpty() || (interface_ && interface_ != scope)) {
                error = "Select the interface for this IPv6 link-local address.";
                return false;
            }
            interface_ = scope;
            address_.setScopeId(interfaceName(scope));
        }
        if (v6_) {
            auto &ip = reinterpret_cast<sockaddr_in6 &>(peer_);
            ip.sin6_len = sizeof(ip);
            ip.sin6_family = AF_INET6;
            ip.sin6_scope_id = address_.isLinkLocal() ? interface_ : 0;
            const auto bytes = address_.toIPv6Address();
            std::memcpy(&ip.sin6_addr, bytes.c, sizeof(ip.sin6_addr));
        } else {
            auto &ip = reinterpret_cast<sockaddr_in &>(peer_);
            ip.sin_len = sizeof(ip);
            ip.sin_family = AF_INET;
            ip.sin_addr.s_addr = htonl(address_.toIPv4Address());
        }
        if (v6_ && options_.dontFragment) {
            error = "Unfragmented IPv6 probes are unavailable on macOS ICMP datagram sockets.";
            return false;
        }

        fd_ = socket(peer_.ss_family, SOCK_DGRAM, v6_ ? int(IPPROTO_ICMPV6) : int(IPPROTO_ICMP));
        const int tos = options_.dscp << 2;
        const int one = 1;
        if (fd_ < 0 || fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd_, F_SETFL, O_NONBLOCK) < 0 ||
            !bindInterface(fd_) ||
            (tos && setsockopt(fd_, level(), v6_ ? IPV6_TCLASS : IP_TOS, &tos, sizeof(tos)) < 0) ||
            (options_.dontFragment && setsockopt(fd_, IPPROTO_IP, IP_DONTFRAG, &one, sizeof(one)) < 0)) {
            error = systemMessage(errno);
            return false;
        }
        // Verify that the selected family is routable before accepting a DNS candidate.
        if (sourceAddress().isNull()) {
            error = "No route to " + address_.toString();
            return false;
        }
        packet_[0] = v6_ ? char(128) : char(8);
        notifier_ = std::make_unique<QSocketNotifier>(fd_, QSocketNotifier::Read);
        QObject::connect(notifier_.get(), &QSocketNotifier::activated, notifier_.get(), [this] { drain(); });
        return true;
    }

    QVariantMap route() const override {
        QVariantMap result;
        const auto source = sourceAddress();
        if (!source.isNull()) result.insert("sourceAddress", source.toString());
        if (interface_) result.insert("effectiveInterface", interfaceName(interface_));
        return result;
    }

    void send(quint64 token, int ttl) override {
        // Darwin delivers ICMP for other ping sockets too. An error may quote only the
        // identifier and sequence; allocate those 32 bits across every local trace.
        static std::atomic<quint32> nextKey{QRandomGenerator::system()->generate()};
        const quint32 key = nextKey.fetch_add(1, std::memory_order_relaxed);
        packet_[2] = packet_[3] = 0;
        qToBigEndian(key, packet_.data() + 4);
        qToBigEndian(cookie_ ^ token, packet_.data() + 8);
        if (!v6_) qToBigEndian(DarwinIcmp::checksum(packet_), packet_.data() + 2);

        const auto sent = Clock::now();
        if (setsockopt(fd_, level(), v6_ ? IPV6_UNICAST_HOPS : IP_TTL, &ttl, sizeof(ttl)) < 0 ||
            sendto(fd_, packet_.constData(), packet_.size(), 0,
                   reinterpret_cast<const sockaddr *>(&peer_), peer_.ss_len) < 0) {
            const int error = errno;
            const auto callback = reply_;
            callback(token, {{}, -2, false, {"local-unsent", {}, -1, -1, 0, error}});
            return;
        }
        pending_.insert(key, {token, sent, ttl});
        keys_.insert(token, key);
    }

    void discard(quint64 token) override {
        const auto it = keys_.find(token);
        if (it != keys_.end()) {
            pending_.remove(*it);
            keys_.erase(it);
        }
    }

private:
    int level() const { return v6_ ? IPPROTO_IPV6 : IPPROTO_IP; }

    bool bindInterface(int fd) const {
        return !interface_ || setsockopt(fd, level(), v6_ ? IPV6_BOUND_IF : IP_BOUND_IF,
                                         &interface_, sizeof(interface_)) == 0;
    }

    QHostAddress sourceAddress() const {
        // A connected UDP socket asks Darwin to choose the source without sending
        // traffic. Connecting the actual ICMP socket would hide intermediate routers.
        const int selection = socket(peer_.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (selection < 0) return {};
        const auto cleanup = qScopeGuard([selection] { close(selection); });
        sockaddr_storage dest = peer_, local{};
        if (v6_) reinterpret_cast<sockaddr_in6 &>(dest).sin6_port = htons(33434);
        else reinterpret_cast<sockaddr_in &>(dest).sin_port = htons(33434);
        auto length = socklen_t(sizeof(local));
        if (fcntl(selection, F_SETFD, FD_CLOEXEC) < 0 || !bindInterface(selection) ||
            ::connect(selection, reinterpret_cast<sockaddr *>(&dest), dest.ss_len) < 0 ||
            getsockname(selection, reinterpret_cast<sockaddr *>(&local), &length) < 0) return {};
        return host(local);
    }

    void drain() {
        // Stop draining if delivery of a reply closed this transport.
        const QPointer<QSocketNotifier> alive(notifier_.get());
        std::array<char, 2048> buffer;
        for (int i = 0; i < 128 && alive; ++i) {
            sockaddr_storage peer{};
            socklen_t length = sizeof(peer);
            const auto size = recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr *>(&peer), &length);
            if (size < 0) {
                if (errno == EINTR) continue;
                return;
            }
            const auto message = DarwinIcmp::parse({buffer.data(), size}, address_);
            if (!message) continue;
            const auto pending = pending_.constFind(message->key);
            if (pending == pending_.cend()) continue;
            const auto router = host(peer);
            if (router.isNull()) continue;
            if (message->echo && (router != address_ || message->cookie != (cookie_ ^ pending->token))) continue;

            EchoReply result;
            if (message->echo || (message->transit && pending->ttl != 255)) {
                result.address = router.toString();
                result.rttMs = std::chrono::duration<double, std::milli>(Clock::now() - pending->sent).count();
                result.destination = message->echo;
            } else {
                const auto type = message->type, code = message->code;
                const bool unreachable = type == (v6_ ? 1 : 3);
                const bool tooBig = v6_ ? type == 2 : unreachable && code == 4;
                const bool filtered = unreachable && (v6_ ? code == 1 : code == 9 || code == 10 || code == 13);
                result.failure = {tooBig ? "packet-too-big" : filtered ? "filtered" : unreachable ? "unreachable" : "icmp-error",
                                  router.toString(), type, code, message->mtu};
            }
            const auto token = pending->token;
            discard(token);
            const auto callback = reply_;
            callback(token, std::move(result));
        }
    }

    struct Pending { quint64 token; Clock::time_point sent; int ttl; };
    QHostAddress address_;
    bool v6_;
    ProbeOptions options_;
    Reply reply_;
    QByteArray packet_;
    quint64 cookie_;
    sockaddr_storage peer_{};
    unsigned interface_ = 0;
    int fd_ = -1;
    std::unique_ptr<QSocketNotifier> notifier_;
    QHash<quint32, Pending> pending_;
    QHash<quint64, quint32> keys_;
};
} // namespace

std::unique_ptr<NativeEcho> openNativeEcho(const QHostAddress &address, const ProbeOptions &options,
                                          NativeEcho::Reply reply, QString &error) {
    auto echo = std::make_unique<MacEcho>(address, options, std::move(reply));
    if (!echo->open(error)) return {};
    return echo;
}
