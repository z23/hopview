#include "probeoptions.h"
#include "probe.h"

#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <linux/rtnetlink.h>
#include <QSocketNotifier>
#include <QTimer>

#include <arpa/inet.h>
#include <linux/errqueue.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <utility>

namespace {
using Clock = std::chrono::steady_clock;
using Moment = Clock::time_point;
using namespace std::chrono_literals;

constexpr int maxHops = 30;
constexpr int burst = 4;
constexpr auto burstSpacing = 12ms;
constexpr auto responseTimeout = 1500ms;
constexpr std::array<unsigned char, 4> udpMarker{'H', 'O', 'P', 'L'};

struct Endpoint {
    sockaddr_storage storage{};
    socklen_t length = 0;

    sockaddr *data() { return reinterpret_cast<sockaddr *>(&storage); }
    const sockaddr *data() const { return reinterpret_cast<const sockaddr *>(&storage); }
    int family() const { return storage.ss_family; }

    bool set(const QHostAddress &address) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            auto &v4 = reinterpret_cast<sockaddr_in &>(storage);
            v4.sin_family = AF_INET;
            v4.sin_addr.s_addr = htonl(address.toIPv4Address());
            length = sizeof(v4);
            return true;
        }
        if (address.protocol() != QAbstractSocket::IPv6Protocol) return false;
        auto &v6 = reinterpret_cast<sockaddr_in6 &>(storage);
        v6.sin6_family = AF_INET6;
        const auto bytes = address.toIPv6Address();
        std::memcpy(&v6.sin6_addr, bytes.c, sizeof(v6.sin6_addr));
        if (!address.scopeId().isEmpty()) {
            bool numeric = false;
            const unsigned index = address.scopeId().toUInt(&numeric);
            v6.sin6_scope_id = numeric ? index : if_nametoindex(address.scopeId().toLocal8Bit().constData());
            if (!v6.sin6_scope_id) return false;
        }
        length = sizeof(v6);
        return !IN6_IS_ADDR_LINKLOCAL(&v6.sin6_addr) || v6.sin6_scope_id != 0;
    }

    void setPort(quint16 port) {
        if (family() == AF_INET) reinterpret_cast<sockaddr_in &>(storage).sin_port = htons(port);
        else reinterpret_cast<sockaddr_in6 &>(storage).sin6_port = htons(port);
    }

    quint16 port() const {
        if (family() == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in &>(storage).sin_port);
        if (family() == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6 &>(storage).sin6_port);
        return 0;
    }
};

QHostAddress hostOf(const sockaddr *sa, quint32 scope = 0) {
    if (sa->sa_family == AF_INET)
        return QHostAddress(ntohl(reinterpret_cast<const sockaddr_in *>(sa)->sin_addr.s_addr));
    if (sa->sa_family != AF_INET6) return {};
    const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(sa);
    QHostAddress host(reinterpret_cast<const quint8 *>(&v6->sin6_addr));
    if (host.isLinkLocal()) {
        const auto index = v6->sin6_scope_id ? v6->sin6_scope_id : scope;
        char name[IF_NAMESIZE]{};
        if (index) host.setScopeId(if_indextoname(index, name) ? QString::fromLatin1(name) : QString::number(index));
    }
    return host;
}

// Ask the routing table which output device and source address match these selectors.
// A source address alone can belong to a different device than the egress route.
struct RouteSelection {
    QString device, source;
};
RouteSelection routedSource(const Endpoint &endpoint, const QString &method, const ProbeOptions &options) {
    RouteSelection result{options.interfaceName, {}};
    const int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) return result;
    struct Request {
        nlmsghdr header;
        rtmsg route;
        std::array<char, 128> attrs;
    } request{};
    const bool v6 = endpoint.family() == AF_INET6;
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request.route.rtm_family = endpoint.family();
    request.route.rtm_dst_len = v6 ? 128 : 32;
    request.route.rtm_tos = options.dscp << 2;
    auto add = [&](int type, const void *value, size_t length) {
        auto *at = reinterpret_cast<char *>(&request) + NLMSG_ALIGN(request.header.nlmsg_len);
        auto *attr = reinterpret_cast<rtattr *>(at);
        attr->rta_type = type;
        attr->rta_len = RTA_LENGTH(length);
        std::memcpy(RTA_DATA(attr), value, length);
        request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) + RTA_ALIGN(attr->rta_len);
    };
    const auto &ip4 = reinterpret_cast<const sockaddr_in &>(endpoint.storage);
    const auto &ip6 = reinterpret_cast<const sockaddr_in6 &>(endpoint.storage);
    if (v6) add(RTA_DST, &ip6.sin6_addr, sizeof(ip6.sin6_addr));
    else add(RTA_DST, &ip4.sin_addr, sizeof(ip4.sin_addr));
    unsigned device = options.interfaceName.isEmpty() ? (v6 ? ip6.sin6_scope_id : 0)
        : if_nametoindex(options.interfaceName.toLocal8Bit().constData());
    if (device) add(RTA_OIF,&device,sizeof(device));
    const quint8 protocol = method == "udp" ? int(IPPROTO_UDP) : method == "tcp" ? int(IPPROTO_TCP)
                              : v6 ? int(IPPROTO_ICMPV6) : int(IPPROTO_ICMP);
    add(RTA_IP_PROTO,&protocol,sizeof(protocol));
    if (method != "icmp") {
        const auto port = htons(endpoint.port());
        add(RTA_DPORT,&port,sizeof(port));
    }
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    alignas(nlmsghdr) std::array<char,4096> answer{};
    ssize_t length = -1;
    if (sendto(fd,&request,request.header.nlmsg_len,0,reinterpret_cast<sockaddr *>(&kernel),sizeof(kernel)) >= 0)
        length = recv(fd,answer.data(),answer.size(),MSG_DONTWAIT);
    close(fd);
    if (length < 0) return result;
    auto *header = reinterpret_cast<nlmsghdr *>(answer.data());
    if (!NLMSG_OK(header,length) || header->nlmsg_type != RTM_NEWROUTE) return result;
    auto *route = static_cast<rtmsg *>(NLMSG_DATA(header));
    int remaining = RTM_PAYLOAD(header);
    for (auto *attr = RTM_RTA(route); RTA_OK(attr,remaining); attr = RTA_NEXT(attr,remaining)) {
        if (attr->rta_type == RTA_OIF && RTA_PAYLOAD(attr) == sizeof(device)) {
            std::memcpy(&device,RTA_DATA(attr),sizeof(device));
            char name[IF_NAMESIZE]{};
            if (if_indextoname(device,name)) result.device = QString::fromLocal8Bit(name);
        } else if (attr->rta_type == RTA_PREFSRC) {
            const auto *bytes = static_cast<const quint8 *>(RTA_DATA(attr));
            if (v6 && RTA_PAYLOAD(attr)==16) result.source = QHostAddress(bytes).toString();
            else if (!v6 && RTA_PAYLOAD(attr)==4) {
                in_addr_t ip{};
                std::memcpy(&ip,bytes,sizeof(ip));
                result.source = QHostAddress(ntohl(ip)).toString();
            }
        }
    }
    return result;
}

struct Round {
    qint64 wallMs = 0;
    int nextHop = 1;
    int limit = maxHops;
    int destination = 0;
    int outstanding = 0;
    Moment nextSend{};
    // Index zero is a separately transmitted endpoint check with an unrestricted hop limit.
    std::array<std::optional<Sample>, maxHops + 1> values{};
};

struct Pending {
    qint64 round;
    int hop;
    Moment sent;
    qint64 wallNs;
};

qint64 realtimeNs() {
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    return qint64(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

QString systemError(int error) { return QString::fromLocal8Bit(strerror(error)); }
} // namespace

class ProbeSession : public QObject {
public:
    ProbeSession(Probe *owner, qint64 id, int intervalMs, ProbeOptions options)
        : QObject(owner), owner_(owner), id_(id), interval_(std::clamp(intervalMs, 250, 3'600'000)),
          options_(std::move(options)) {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    }
    ~ProbeSession() override { cancel(); }

    void cancel() {
        active_ = false;
        if (lookup_ >= 0) QHostInfo::abortHostLookup(std::exchange(lookup_, -1));
        timer_.stop();
        if (notifier_) notifier_->setEnabled(false);
        if (fd_ >= 0) ::close(std::exchange(fd_, -1));
        while (!sockets_.empty()) closeSocket(sockets_.begin()->first);
    }

    void finish() {
        finishing_ = true;
        if (rounds_.empty()) {
            cancel();
            emit owner_->finished(id_);
        } else {
            tick();
        }
    }

    void resolve(QString target, int family) {
        target = target.trimmed();
        if (target.isEmpty() || (family != 0 && family != 4 && family != 6)) {
            fail("Specify a host or address and a valid IP family.");
            return;
        }
        if (!options_.valid()) {
            fail("Invalid probe protocol or port.");
            return;
        }
        if (!options_.interfaceName.isEmpty() && !if_nametoindex(options_.interfaceName.toLocal8Bit().constData())) {
            fail("Unknown network interface: " + options_.interfaceName);
            return;
        }
        QHostAddress literal;
        if (literal.setAddress(target)) {
            select({literal}, family);
            return;
        }
        emit owner_->status(id_, "Resolving " + target, false);
        lookup_ = QHostInfo::lookupHost(target, this, [this, family](const QHostInfo &info) {
            lookup_ = -1;
            if (!active_) return;
            if (info.error() != QHostInfo::NoError) {
                fail(info.errorString());
                return;
            }
            select(info.addresses(), family);
        });
    }

private:
    void fail(QString message) {
        owner_->stop(id_);
        emit owner_->status(id_, message, true);
    }

    bool bindInterface(int fd) const {
        if (options_.interfaceName.isEmpty()) return true;
        const auto name = options_.interfaceName.toLocal8Bit();
        return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name.constData(), name.size() + 1) == 0;
    }

    int openSocket(int family, const QString &protocol) const {
        const bool v6 = family == AF_INET6;
        const bool tcp = protocol == "tcp";
        const int type = tcp ? SOCK_STREAM : SOCK_DGRAM;
        const int transport = tcp ? int(IPPROTO_TCP) : protocol == "udp" ? int(IPPROTO_UDP)
                                       : v6 ? int(IPPROTO_ICMPV6) : int(IPPROTO_ICMP);
        const int fd = ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, transport);
        if (fd < 0) return -1;
        const int on = 1;
        if (!bindInterface(fd) ||
            setsockopt(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_RECVERR : IP_RECVERR, &on, sizeof(on)) < 0) {
            const int error = errno;
            ::close(fd);
            errno = error;
            return -1;
        }
        const int tos = options_.dscp << 2;
        const int level = v6 ? IPPROTO_IPV6 : IPPROTO_IP;
        const int discover = IP_PMTUDISC_DO;
        if ((tos && setsockopt(fd, level, v6 ? IPV6_TCLASS : IP_TOS, &tos, sizeof(tos)) < 0) ||
            (options_.dontFragment && setsockopt(fd, level, v6 ? IPV6_MTU_DISCOVER : IP_MTU_DISCOVER,
                                               &discover, sizeof(discover)) < 0)) {
            const int error = errno;
            ::close(fd);
            errno = error;
            return -1;
        }
        setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
        if (v6) setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));
        return fd;
    }

    QString description() const {
        QString result = target_.toString() + " via " + method_.toUpper();
        if (method_ != "icmp") result += "/" + QString::number(endpoint_.port());
        if (!options_.interfaceName.isEmpty()) result += " on " + options_.interfaceName;
        return result;
    }

    void select(const QList<QHostAddress> &addresses, int family) {
        QString error = "No address for the selected IP family.";
        const QStringList methods = options_.protocol == "auto"
            ? QStringList{"icmp", "udp"} : QStringList{options_.protocol};
        for (auto address : addresses) {
            if ((family == 4 && address.protocol() != QAbstractSocket::IPv4Protocol)
                || (family == 6 && address.protocol() != QAbstractSocket::IPv6Protocol)) continue;
            if (address.protocol() == QAbstractSocket::IPv6Protocol && address.isLinkLocal()
                && address.scopeId().isEmpty()) address.setScopeId(options_.interfaceName);
            Endpoint endpoint;
            if (!endpoint.set(address)) {
                error = "IPv6 link-local targets need an interface (for example %eth0).";
                continue;
            }
            endpoint.setPort(options_.destinationPort() ? options_.destinationPort() : 33434);
            const int route = ::socket(endpoint.family(), SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
            if (route < 0 || !bindInterface(route) || ::connect(route, endpoint.data(), endpoint.length) < 0) {
                error = systemError(errno);
                if (route >= 0) ::close(route);
                continue;
            }
            ::close(route);
            for (const auto &method : methods) {
                int fd = openSocket(endpoint.family(), method);
                if (fd < 0) {
                    error = systemError(errno);
                    continue;
                }
                if (method == "udp" && ::connect(fd, endpoint.data(), endpoint.length) < 0) {
                    error = systemError(errno);
                    ::close(fd);
                    continue;
                }
                method_ = method;
                endpoint_ = endpoint;
                if (method == "icmp") endpoint_.setPort(0);
                target_ = hostOf(endpoint_.data());
                if (method == "tcp") {
                    ::close(fd); // Each in-flight connection has its own SYN and error queue.
                } else {
                    fd_ = fd;
                    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
                    connect(notifier_, &QSocketNotifier::activated, this, [this] {
                        drain();
                        finishRounds();
                    });
                }
                packet_.resize(options_.payloadBytes + (method == "icmp" ? 8 : 0));
                packet_.fill(0);
                actual_ = {{"resolvedAddress",target_.toString()}, {"effectiveFamily",endpoint.family()==AF_INET ? 4 : 6},
                           {"effectiveProtocol",method_}, {"effectivePort",endpoint_.port()},
                           {"flowMode",method_ == "udp" ? "fixed-udp" : method_ == "tcp" ? "separate-ports" : "icmp"}};
                refreshRoute();
                emit owner_->resolved(id_, target_.toString(), method_, endpoint_.port());
                emit owner_->status(id_, description(), false);
                nextRound_ = Clock::now();
                tick();
                return;
            }
        }
        fail("Cannot trace target: " + error);
    }

    void refreshRoute() {
        const auto route = routedSource(endpoint_,method_,options_);
        if (!actual_.contains("sourceAddress") || actual_["sourceAddress"] != route.source ||
            actual_["effectiveInterface"] != route.device) {
            actual_["sourceAddress"] = route.source;
            actual_["effectiveInterface"] = route.device;
            emit owner_->measurement(id_,QDateTime::currentMSecsSinceEpoch(),actual_);
        }
        nextRouteCheck_ = Clock::now() + 5s;
    }
    void tick();
    void sendProbe(qint64 roundId, Round &round, int hop);
    void drain();
    bool receive(int fd, bool errors, std::optional<quint16> socketKey = {});
    void switchUdpSockets();
    void separateProbe(qint64 number, Round &r, int hop, quint16 key);
    void socketReady(quint16 key);
    void closeSocket(quint16 key);
    void sendError(Round &round, int hop, int error);
    void complete(quint16 key, const QHostAddress &responder, bool destination, qint64 rxNs);
    void finishRounds();

    Probe *owner_;
    qint64 id_;
    std::chrono::milliseconds interval_;
    bool active_ = true;
    bool finishing_ = false;
    int fd_ = -1;
    int lookup_ = -1;
    ProbeOptions options_;
    QString method_;
    QVariantMap actual_;
    QByteArray packet_;
    Moment nextRouteCheck_{};
    bool separateUdp_ = false;
    struct Socket { int fd; QSocketNotifier *notifier; };
    std::map<quint16, Socket> sockets_;
    Endpoint endpoint_;
    QHostAddress target_;
    QSocketNotifier *notifier_ = nullptr;
    QTimer timer_;
    int hopLimit_ = maxHops;
    unsigned sequence_ = 0;
    qint64 roundId_ = 0;
    Moment nextRound_{};
    Moment lastError_{};
    std::map<qint64, Round> rounds_;
    std::map<quint16, Pending> pending_;
};

void ProbeSession::tick() {
    if (!active_) return;
    drain();
    const auto now = Clock::now();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.sent + responseTimeout > now) { ++it; continue; }
        const auto &probe = it->second;
        auto &round = rounds_.at(probe.round);
        round.values[probe.hop] = Sample{id_, round.wallMs, probe.hop, {}, -1, false};
        --round.outstanding;
        closeSocket(it->first);
        it = pending_.erase(it);
    }
    finishRounds();
    if (!active_) return;
    if (!finishing_ && now >= nextRound_) {
        if (now >= nextRouteCheck_) refreshRoute();
        auto &r = rounds_[++roundId_];
        r.wallMs = QDateTime::currentMSecsSinceEpoch();
        r.limit = hopLimit_;
        r.nextSend = now;
        // A local send failure leaves the endpoint unmeasured. A successful send
        // replaces this placeholder with a reply or a recorded timeout.
        r.values[0] = Sample{id_, r.wallMs, 0, {}, -2, false};
        sendProbe(roundId_, r, 0);
        nextRound_ += interval_;
        if (nextRound_ <= now)
            nextRound_ = now + interval_; // Missed intervals do not produce catch-up traffic.
    }
    auto wake = finishing_ ? now + responseTimeout : nextRound_;
    for (auto &[number, r] : rounds_) {
        if (r.nextHop > r.limit) continue;
        if (now >= r.nextSend) {
            for (int sent = 0; sent < burst && r.nextHop <= r.limit; ++sent) {
                const int hop = r.nextHop++;
                sendProbe(number, r, hop);
            }
            r.nextSend = now + burstSpacing;
        }
        if (r.nextHop <= r.limit) wake = std::min(wake, r.nextSend);
    }
    for (const auto &[key, probe] : pending_)
        wake = std::min(wake, probe.sent + responseTimeout);
    finishRounds();
    if (!active_) return;
    timer_.start(std::max(1ms, std::chrono::ceil<std::chrono::milliseconds>(wake - Clock::now())));
}

void ProbeSession::sendProbe(qint64 number, Round &r, int hop) {
    const bool ipv6 = endpoint_.family() == AF_INET6;
    const int level = ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
    const int option = ipv6 ? IPV6_UNICAST_HOPS : IP_TTL;
    const quint16 key = quint16(++sequence_);
    const int ttl = hop ? hop : 255;
    if (method_ == "tcp" || separateUdp_) {
        separateProbe(number, r, hop, key);
        return;
    }
    auto &packet = packet_;
    const auto sequence = htons(key);
    if (method_ == "udp") {
        std::copy(udpMarker.begin(), udpMarker.end(), packet.begin());
        std::memcpy(&packet[4], &sequence, sizeof(sequence));
        // Keep the checksum stable as well as the UDP five-tuple across hops.
        const quint16 complement = ~sequence;
        std::memcpy(&packet[6], &complement, sizeof(complement));
    } else {
        packet[0] = ipv6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO;
        std::memcpy(&packet[6], &sequence, sizeof(sequence));
    }
    // The kernel fills in the identifier and checksum on ICMP echo sockets.
    qint64 wall = 0;
    Moment sent{};
    if (setsockopt(fd_, level, option, &ttl, sizeof(ttl)) == 0) {
        const auto send = [&] {
            wall = realtimeNs();
            sent = Clock::now();
            return sendto(fd_, packet.data(), packet.size(), 0, endpoint_.data(), endpoint_.length);
        };
        auto result = send();
        if (result < 0) {
            // An earlier probe's asynchronous error can be returned by a later send.
            drain();
            if (separateUdp_) {
                separateProbe(number, r, hop, key);
                return;
            }
            result = send();
        }
        if (result == qint64(packet.size())) {
            pending_.emplace(key, Pending{number, hop, sent, wall});
            ++r.outstanding;
            return;
        }
    }
    sendError(r, hop, errno);
}

void ProbeSession::sendError(Round &r, int hop, int error) {
    r.values[hop] = Sample{id_, r.wallMs, hop, {}, -2, false, {"local-unsent", {}, -1, -1, 0, error}};
    const auto now = Clock::now();
    if (now - lastError_ > 3s) {
        lastError_ = now;
        emit owner_->status(id_, "Probe could not be sent: " + systemError(error), false);
    }
    // A local send failure did not put a probe on the wire.
}

void ProbeSession::switchUdpSockets() {
    if (separateUdp_) return;
    separateUdp_ = true;
    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (fd_ >= 0) ::close(std::exchange(fd_, -1));
    actual_["flowMode"] = "separate-ports";
    emit owner_->measurement(id_,QDateTime::currentMSecsSinceEpoch(),actual_);
    emit owner_->status(id_, description() + " (using separate source ports for untagged replies)", false);
    // A short quote (or application response) has no sequence number. Retry in-flight
    // measurements on independent sockets, starting a fresh RTT clock for each one.
    auto untagged = std::exchange(pending_, {});
    for (const auto &[key, p] : untagged) {
        auto &r = rounds_.at(p.round);
        --r.outstanding;
        if (p.hop <= r.limit) separateProbe(p.round, r, p.hop, key);
    }
}

void ProbeSession::separateProbe(qint64 number, Round &r, int hop, quint16 key) {
    const bool tcp = method_ == "tcp";
    const bool v6 = endpoint_.family() == AF_INET6;
    const int fd = openSocket(endpoint_.family(), method_);
    if (fd < 0) { sendError(r, hop, errno); return; }
    const auto reject = [this, fd, &r, hop](int error) {
        ::close(fd);
        sendError(r, hop, error);
    };
    const int ttl = hop ? hop : 255;
    if (setsockopt(fd, v6 ? IPPROTO_IPV6 : IPPROTO_IP, v6 ? IPV6_UNICAST_HOPS : IP_TTL, &ttl, sizeof(ttl)) < 0) {
        reject(errno);
        return;
    }
    const auto wall = realtimeNs();
    const auto sent = Clock::now();
    const int connected = ::connect(fd, endpoint_.data(), endpoint_.length);
    const int connectError = connected < 0 ? errno : 0;
    if (connectError && !(tcp && (connectError == EINPROGRESS || connectError == ECONNREFUSED))) {
        reject(connectError);
        return;
    }
    if (!tcp) {
        // The socket identifies this probe even for an empty ICMP quote or an application-specific response.
        if (::send(fd, packet_.constData(), packet_.size(), 0) < 0) { reject(errno); return; }
    }
    pending_.emplace(key, Pending{number, hop, sent, wall});
    ++r.outstanding;
    if (tcp && (connected == 0 || connectError == ECONNREFUSED)) {
        ::close(fd);
        complete(key, target_, true, 0);
        return;
    }
    auto *notifier = new QSocketNotifier(fd, tcp ? QSocketNotifier::Write : QSocketNotifier::Read, this);
    sockets_.emplace(key, Socket{fd, notifier});
    connect(notifier, &QSocketNotifier::activated, this, [this, key] { socketReady(key); });
}

void ProbeSession::closeSocket(quint16 key) {
    auto it = sockets_.find(key);
    if (it == sockets_.end()) return;
    it->second.notifier->setEnabled(false);
    it->second.notifier->deleteLater();
    ::close(it->second.fd);
    sockets_.erase(it);
}

void ProbeSession::socketReady(quint16 key) {
    const auto it = sockets_.find(key);
    if (it == sockets_.end()) return;
    const int fd = it->second.fd;
    if (method_ == "tcp") {
        // The kernel queues ICMP errors with the responding router while the connection is pending.
        for (int count = 0; count < 16 && sockets_.contains(key) && receive(fd, true, key); ++count) {}
        if (sockets_.contains(key)) {
            int error = 0;
            socklen_t len = sizeof(error);
            const bool gotError = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0;
            sockaddr_storage peer{};
            socklen_t peerLen = sizeof(peer);
            const bool connected = gotError && error == 0
                && ::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &peerLen) == 0;
            if (gotError && (connected || error == ECONNREFUSED)) {
                complete(key, target_, true, 0);
            } else {
                // Filtering or routing failures provide no successful RTT. Leave the pending
                // sample until its deadline, but retire this write-ready descriptor now.
                closeSocket(key);
            }
        }
    } else {
        receive(fd, true, key);
        if (sockets_.contains(key)) receive(fd, false, key);
    }
    finishRounds();
}

void ProbeSession::drain() {
    if (fd_ < 0) return;
    // Bound each callback so queued responses cannot starve other sessions or controls.
    for (const bool errors : {true, false})
        for (int count = 0; count < 256 && fd_ >= 0 && receive(fd_, errors); ++count) {}
}

bool ProbeSession::receive(int fd, bool errors, std::optional<quint16> socketKey) {
    std::array<unsigned char, 256> packet{};
    alignas(cmsghdr) std::array<unsigned char, 512> control{};
    Endpoint peer;
    iovec data{packet.data(), packet.size()};
    msghdr msg{};
    msg.msg_name = peer.data();
    msg.msg_namelen = sizeof(peer.storage);
    msg.msg_iov = &data;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    const auto size = recvmsg(fd, &msg, MSG_DONTWAIT | (errors ? MSG_ERRQUEUE : 0));
    if (size < 0) return errno == EINTR;
    const bool v6 = endpoint_.family() == AF_INET6;
    sock_extended_err *err = nullptr;
    const sockaddr *offender = nullptr;
    qint64 rxNs = 0;
    quint32 scope = 0;
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS && c->cmsg_len >= CMSG_LEN(sizeof(timespec))) {
            timespec ts{};
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            rxNs = qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
        }
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo)))
            scope = reinterpret_cast<const in6_pktinfo *>(CMSG_DATA(c))->ipi6_ifindex;
        if (c->cmsg_level == (v6 ? IPPROTO_IPV6 : IPPROTO_IP) && c->cmsg_type == (v6 ? IPV6_RECVERR : IP_RECVERR)
            && c->cmsg_len >= CMSG_LEN(sizeof(sock_extended_err))) {
            err = reinterpret_cast<sock_extended_err *>(CMSG_DATA(c));
            if (c->cmsg_len >= CMSG_LEN(sizeof(*err) + (v6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in))))
                offender = SO_EE_OFFENDER(err);
        }
    }
    const auto peerAddress = hostOf(peer.data(), scope);
    if (peerAddress != target_) return true;
    quint16 key = 0;
    if (socketKey) {
        key = *socketKey;
    } else if (method_ == "icmp") {
        if (size < 8) return true;
        const int expectedType = errors ? (v6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO)
                                        : (v6 ? ICMP6_ECHO_REPLY : ICMP_ECHOREPLY);
        if (packet[0] != expectedType || packet[1] != 0) return true;
        std::memcpy(&key, &packet[6], sizeof(key));
        key = ntohs(key);
    } else {
        if (separateUdp_ || peer.port() != endpoint_.port()) return true;
        if (size < 8 || !std::equal(udpMarker.begin(), udpMarker.end(), packet.begin())) {
            // A minimal ICMP quote ends at the UDP header. Its payload cannot identify
            // a particular TTL on the shared flow.
            switchUdpSockets();
            return true;
        }
        std::memcpy(&key, &packet[4], sizeof(key));
        key = ntohs(key);
    }
    if (!errors) {
        // In UDP mode an answering application also proves the destination was reached.
        complete(key, peerAddress, true, rxNs);
        return true;
    }
    if (!err || !offender || err->ee_origin != (v6 ? SO_EE_ORIGIN_ICMP6 : SO_EE_ORIGIN_ICMP)) return true;
    const auto router = hostOf(offender, scope);
    if (router.isNull()) return true;
    const bool expired = err->ee_type == (v6 ? ICMP6_TIME_EXCEEDED : ICMP_TIME_EXCEEDED) && err->ee_code == 0;
    const bool portUnreachable = err->ee_type == (v6 ? ICMP6_DST_UNREACH : ICMP_DEST_UNREACH)
                              && err->ee_code == (v6 ? ICMP6_DST_UNREACH_NOPORT : ICMP_PORT_UNREACH);
    if ((expired && pending_.contains(key) && pending_.at(key).hop != 0) || (method_ == "udp" && portUnreachable && router == target_)) {
        complete(key, router, !expired, rxNs);
    } else if (auto pending = pending_.find(key); pending != pending_.end()) {
        const bool tooBig = v6 ? err->ee_type == ICMP6_PACKET_TOO_BIG
            : err->ee_type == ICMP_DEST_UNREACH && err->ee_code == ICMP_FRAG_NEEDED;
        const bool unreachable = err->ee_type == (v6 ? ICMP6_DST_UNREACH : ICMP_DEST_UNREACH);
        const bool filtered = unreachable && (v6 ? err->ee_code == ICMP6_DST_UNREACH_ADMIN
            : err->ee_code == ICMP_PKT_FILTERED || err->ee_code == ICMP_NET_ANO || err->ee_code == ICMP_HOST_ANO);
        const auto kind = tooBig ? "packet-too-big" : filtered ? "filtered" : unreachable ? "unreachable" : "icmp-error";
        const auto hop = pending->second.hop;
        auto &r = rounds_.at(pending->second.round);
        r.values[hop] = Sample{id_, r.wallMs, hop, {}, -1, false,
            {kind, router.toString(), err->ee_type, err->ee_code, tooBig ? int(err->ee_info) : 0, int(err->ee_errno)}};
        --r.outstanding;
        closeSocket(key);
        pending_.erase(pending);
    }
    return true;
}

void ProbeSession::complete(quint16 key, const QHostAddress &responder, bool destination, qint64 rxNs) {
    auto it = pending_.find(key);
    if (it == pending_.end()) return;
    const auto &p = it->second;
    const qint64 elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - p.sent).count();
    const qint64 wallElapsed = realtimeNs() - p.wallNs;
    // Use the kernel arrival timestamp when the wall clock has remained coherent.
    // A clock step or absent timestamp falls back to the monotonic measurement.
    const qint64 kernelElapsed = rxNs - p.wallNs;
    const bool valid = kernelElapsed >= 0 && kernelElapsed <= wallElapsed && std::abs(wallElapsed - elapsed) < 2'000'000;
    const double rtt = double(valid ? kernelElapsed : elapsed) / 1e6;
    auto &r = rounds_.at(p.round);
    const bool timely = rtt <= std::chrono::duration<double, std::milli>(responseTimeout).count();
    r.values[p.hop] = Sample{id_, r.wallMs, p.hop, timely ? responder.toString() : QString{},
                                 timely ? rtt : -1.0, timely && destination};
    if (timely && owner_->previews(id_)) emit owner_->replyReceived(id_, r.wallMs, p.hop, responder.toString(), rtt);
    if (timely && destination && p.hop > 0) {
        r.destination = r.destination ? std::min(r.destination, p.hop) : p.hop;
        r.limit = r.destination;
    }
    --r.outstanding;
    closeSocket(key);
    pending_.erase(it);
}

void ProbeSession::finishRounds() {
    QVector<Sample> batch;
    while (!rounds_.empty()) {
        const auto &r = rounds_.begin()->second;
        if (r.nextHop <= r.limit || r.outstanding != 0) break;
        hopLimit_ = r.destination ? r.destination : maxHops;
        for (int h = 0; h <= r.limit; ++h)
            if (r.values[h]) batch.append(*r.values[h]);
        rounds_.erase(rounds_.begin());
    }
    if (!batch.isEmpty()) emit owner_->samples(std::move(batch));
    if (finishing_ && active_ && rounds_.empty()) {
        cancel();
        emit owner_->finished(id_);
    }
}

Probe::Probe(QObject *parent) : QObject(parent) {}

void Probe::start(qint64 id, QString target, int family, int intervalMs, QString protocol, int port, QString interfaceName, int payloadBytes, int dscp, bool dontFragment) {
    stop(id);
    auto *s = new ProbeSession(this, id, intervalMs, {std::move(protocol), port, std::move(interfaceName), payloadBytes, dscp, dontFragment});
    sessions_.insert(id, s);
    s->resolve(std::move(target), family);
}

void Probe::stop(qint64 id) {
    if (auto *s = sessions_.take(id)) {
        s->cancel();
        s->deleteLater();
    }
}

void Probe::finish(qint64 id) {
    if (auto *s = sessions_.value(id)) s->finish();
}

void Probe::shutdown() {
    for (auto id : sessions_.keys()) stop(id);
}
