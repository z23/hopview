#include "probe.h"

#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
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
constexpr int udpBasePort = 33434;

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

struct Round {
    qint64 wallMs = 0;
    int nextHop = 1;
    int limit = maxHops;
    int destination = 0;
    int outstanding = 0;
    Moment nextSend{};
    std::array<std::optional<Sample>, maxHops> values{};
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
    ProbeSession(Probe *owner, qint64 id, int intervalMs)
        : QObject(owner), owner_(owner), id_(id), interval_(std::clamp(intervalMs, 250, 3'600'000)) {
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

    void select(const QList<QHostAddress> &addresses, int family) {
        QString error = "No address for the selected IP family.";
        for (const auto &address : addresses) {
            if ((family == 4 && address.protocol() != QAbstractSocket::IPv4Protocol)
                || (family == 6 && address.protocol() != QAbstractSocket::IPv6Protocol)) continue;
            Endpoint endpoint;
            if (!endpoint.set(address)) {
                error = "IPv6 link-local targets need a valid interface scope (for example %eth0).";
                continue;
            }
            const bool ipv6 = endpoint.family() == AF_INET6;
            const int level = ipv6 ? IPPROTO_IPV6 : IPPROTO_IP;
            for (const bool udp : {false, true}) {
                const int icmpProtocol = ipv6 ? int(IPPROTO_ICMPV6) : int(IPPROTO_ICMP);
                const int fd = ::socket(endpoint.family(), SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                        udp ? int(IPPROTO_UDP) : icmpProtocol);
                if (fd < 0) {
                    error = systemError(errno);
                    continue;
                }
                const int on = 1;
                const auto option = ipv6 ? IPV6_RECVERR : IP_RECVERR;
                if (setsockopt(fd, level, option, &on, sizeof(on)) < 0) {
                    error = systemError(errno);
                    ::close(fd);
                    continue;
                }
                // A temporary connected socket selects and checks the route without filtering
                // our measurement socket's error queue by destination port in UDP mode.
                const int route = ::socket(endpoint.family(), SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
                endpoint.setPort(udpBasePort);
                if (route < 0 || ::connect(route, endpoint.data(), endpoint.length) < 0) {
                    error = systemError(errno);
                    if (route >= 0) ::close(route);
                    ::close(fd);
                    break;
                }
                ::close(route);
                setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
                if (ipv6) setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));
                endpoint_ = endpoint;
                target_ = hostOf(endpoint_.data());
                udp_ = udp;
                fd_ = fd;
                notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
                connect(notifier_, &QSocketNotifier::activated, this, [this] {
                    drain();
                    finishRounds();
                });
                emit owner_->status(id_, QString("%1 via %2").arg(target_.toString(), udp ? "UDP" : "ICMP"), false);
                nextRound_ = Clock::now();
                tick();
                return;
            }
        }
        fail("Cannot trace target: " + error);
    }

    void tick();
    void sendProbe(qint64 roundId, Round &round, int hop);
    void drain();
    bool receive(bool errors);
    void complete(quint16 key, const QHostAddress &responder, bool destination, qint64 rxNs);
    void finishRounds();

    Probe *owner_;
    qint64 id_;
    std::chrono::milliseconds interval_;
    bool active_ = true;
    bool finishing_ = false;
    int fd_ = -1;
    int lookup_ = -1;
    bool udp_ = false;
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
        round.values[probe.hop - 1] = Sample{id_, round.wallMs, probe.hop, {}, -1, false};
        --round.outstanding;
        it = pending_.erase(it);
    }
    finishRounds();
    if (!active_) return;
    if (!finishing_ && now >= nextRound_) {
        auto &r = rounds_[++roundId_];
        r.wallMs = QDateTime::currentMSecsSinceEpoch();
        r.limit = hopLimit_;
        r.nextSend = now;
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
    const quint16 key = udp_ ? quint16(udpBasePort + (++sequence_ % (65536 - udpBasePort)))
                             : quint16(++sequence_);
    std::array<unsigned char, 16> packet{};
    if (udp_) {
        endpoint_.setPort(key);
    } else {
        endpoint_.setPort(0);
        packet[0] = ipv6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO;
        const auto sequence = htons(key);
        std::memcpy(&packet[6], &sequence, sizeof(sequence));
    }
    // Linux fills in the identifier and checksum on ping sockets.
    qint64 wall = 0;
    Moment sent{};
    if (setsockopt(fd_, level, option, &hop, sizeof(hop)) == 0) {
        const auto send = [&] {
            wall = realtimeNs();
            sent = Clock::now();
            return sendto(fd_, packet.data(), packet.size(), 0, endpoint_.data(), endpoint_.length);
        };
        auto result = send();
        if (result < 0) {
            // An earlier probe's asynchronous error can be returned by a later send.
            drain();
            result = send();
        }
        if (result == qint64(packet.size())) {
            pending_.emplace(key, Pending{number, hop, sent, wall});
            ++r.outstanding;
            return;
        }
    }
    const int error = errno;
    const auto now = Clock::now();
    if (now - lastError_ > 3s) {
        lastError_ = now;
        emit owner_->status(id_, "Probe could not be sent: " + systemError(error), false);
    }
    // A local send failure did not put a probe on the wire.
}

void ProbeSession::drain() {
    // Bound each callback so queued responses cannot starve other sessions or controls.
    for (const bool errors : {true, false})
        for (int count = 0; count < 256 && receive(errors); ++count) {}
}

bool ProbeSession::receive(bool errors) {
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
    const auto size = recvmsg(fd_, &msg, MSG_DONTWAIT | (errors ? MSG_ERRQUEUE : 0));
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
    quint16 key = peer.port();
    if (!udp_) {
        if (size < 8) return true;
        const int expectedType = errors ? (v6 ? ICMP6_ECHO_REQUEST : ICMP_ECHO)
                                        : (v6 ? ICMP6_ECHO_REPLY : ICMP_ECHOREPLY);
        if (packet[0] != expectedType || packet[1] != 0) return true;
        std::memcpy(&key, &packet[6], sizeof(key));
        key = ntohs(key);
    }
    const auto peerAddress = hostOf(peer.data(), scope);
    if (peerAddress != target_) return true;
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
    if (expired || (udp_ && portUnreachable && router == target_)) {
        complete(key, router, !expired, rxNs);
    } else if (pending_.contains(key) && Clock::now() - lastError_ > 3s) {
        // An explicit routing or filtering error is not a successful RTT.
        lastError_ = Clock::now();
        emit owner_->status(id_, QString("ICMP error %1/%2 from %3: %4")
                                .arg(err->ee_type).arg(err->ee_code).arg(router.toString(), systemError(err->ee_errno)), false);
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
    r.values[p.hop - 1] = Sample{id_, r.wallMs, p.hop, timely ? responder.toString() : QString{},
                                 timely ? rtt : -1.0, timely && destination};
    if (timely && destination) {
        r.destination = r.destination ? std::min(r.destination, p.hop) : p.hop;
        r.limit = r.destination;
    }
    --r.outstanding;
    pending_.erase(it);
}

void ProbeSession::finishRounds() {
    QVector<Sample> batch;
    while (!rounds_.empty()) {
        const auto &r = rounds_.begin()->second;
        if (r.nextHop <= r.limit || r.outstanding != 0) break;
        hopLimit_ = r.destination ? r.destination : maxHops;
        for (int h = 0; h < r.limit; ++h)
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

void Probe::start(qint64 id, QString target, int family, int intervalMs) {
    stop(id);
    auto *s = new ProbeSession(this, id, intervalMs);
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
