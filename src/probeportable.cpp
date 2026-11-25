#include "probesession.h"
#include "nativeecho.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostInfo>
#include <QTimer>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <utility>

namespace {
constexpr int maxHops = 30;
constexpr int responseTimeoutMs = 1500;
constexpr int burst = 4;
constexpr int burstSpacingMs = 12;

struct Round {
    qint64 stamp = 0;
    qint64 nextSend = 0;
    int nextHop = 1;
    int limit = maxHops;
    int destination = 0;
    int outstanding = 0;
    std::array<std::optional<Sample>, maxHops + 1> values;
};

struct Pending {
    quint64 round;
    int hop;
    qint64 deadline;
};

class EchoSession final : public ProbeSession {
public:
    EchoSession(Probe *owner, qint64 id, int intervalMs, ProbeOptions options)
        : ProbeSession(owner), owner_(owner), id_(id), options_(std::move(options)),
          interval_(std::clamp(intervalMs, 250, 3600000)) {
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    }

    void resolve(QString target, int family) override {
        target = target.trimmed();
        if (target.isEmpty() || (family != 0 && family != 4 && family != 6))
            return fail(tr("Specify a hostname or address and a valid IP family."));
        const auto unsupported = localProbeError(options_, family);
        if (!unsupported.isEmpty())
            return fail(unsupported);
        QHostAddress literal;
        if (literal.setAddress(target)) {
            select({literal}, family);
            return;
        }
        emit owner_->status(id_, tr("Resolving %1").arg(target), false);
        lookup_ = QHostInfo::lookupHost(target, this, [this, family](const QHostInfo &info) {
            lookup_ = -1;
            if (!active_) return;
            if (info.error() != QHostInfo::NoError) return fail(info.errorString());
            select(info.addresses(), family);
        });
    }

    void cancel() override {
        active_ = false;
        timer_.stop();
        if (lookup_ >= 0) QHostInfo::abortHostLookup(std::exchange(lookup_, -1));
        for (const auto &[token, request] : pending_)
            transport_->discard(token);
        pending_.clear();
        // Signals can stop this session during a synchronous send. Leave its
        // current round alive until Probe::stop's deferred destruction.
    }

    void finish() override {
        finishing_ = true;
        if (rounds_.empty()) {
            cancel();
            emit owner_->finished(id_);
        } else {
            timer_.start(0);
        }
    }

private:
    void fail(const QString &message) {
        owner_->stop(id_);
        emit owner_->status(id_, message, true);
    }

    void select(const QList<QHostAddress> &addresses, int family) {
        QString error = tr("No address for the selected IP family.");
        for (auto address : addresses) {
            const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
            if ((family == 6 && !ipv6) || (family == 4 && ipv6)) continue;
            if (ipv6 && address.isLinkLocal() && address.scopeId().isEmpty()) {
                if (options_.interfaceName.isEmpty()) {
                    error = tr("An IPv6 link-local target needs a scope or an outgoing interface.");
                    continue;
                }
                address.setScopeId(options_.interfaceName);
            }
            transport_ = openNativeEcho(address, options_, [this](quint64 token, EchoReply reply) {
                receive(token, std::move(reply));
            }, error);
            if (!transport_) continue;
            actual_ = {{"resolvedAddress", address.toString()}, {"effectiveFamily", ipv6 ? 6 : 4},
                       {"effectiveProtocol", "icmp"}, {"effectivePort", 0}, {"flowMode", "icmp"}};
            clock_.start();
            emit owner_->resolved(id_, address.toString(), "icmp", 0);
            if (!active_) return;
            emit owner_->status(id_, tr("%1 via ICMP").arg(address.toString()), false);
            tick();
            return;
        }
        fail(tr("Cannot trace target: %1").arg(error));
    }

    void receive(quint64 token, EchoReply reply) {
        const auto it = pending_.find(token);
        if (it == pending_.end()) return;
        const auto request = it->second;
        pending_.erase(it);
        auto &r = rounds_.at(request.round);
        --r.outstanding;
        if (reply.rttMs > responseTimeoutMs) reply = {};
        if (request.hop == 0 && reply.rttMs >= 0 && !reply.destination)
            reply = {{}, -1, false, {"icmp-error", reply.address}};
        r.values[request.hop] = Sample{id_, r.stamp, request.hop, reply.address, reply.rttMs,
                                      reply.destination, reply.failure};
        if (reply.destination && request.hop > 0) {
            r.destination = r.destination ? std::min(r.destination, request.hop) : request.hop;
            r.limit = r.destination;
        }
        if (!insideTick_) timer_.start(0);
        if (reply.rttMs >= 0 && owner_->previews(id_))
            emit owner_->replyReceived(id_, r.stamp, request.hop, reply.address, reply.rttMs);
    }

    void send(quint64 number, Round &round, int hop) {
        ++round.outstanding;
        const auto token = ++token_;
        pending_.emplace(token, Pending{number, hop, clock_.elapsed() + responseTimeoutMs});
        transport_->send(token, hop ? hop : 255);
    }

    void flush() {
        QVector<Sample> batch;
        while (!rounds_.empty()) {
            const auto &r = rounds_.begin()->second;
            if (r.nextHop <= r.limit || r.outstanding) break;
            limit_ = r.destination ? r.destination : maxHops;
            for (int hop = 0; hop <= r.limit; ++hop)
                if (r.values[hop]) batch.append(*r.values[hop]);
            rounds_.erase(rounds_.begin());
        }
        if (!batch.isEmpty()) emit owner_->samples(std::move(batch));
    }

    void tick() {
        if (!active_) return;
        insideTick_ = true;
        const auto now = clock_.elapsed();
        while (!pending_.empty() && pending_.begin()->second.deadline <= now) {
            const auto token = pending_.begin()->first;
            transport_->discard(token);
            receive(token, {});
        }
        flush();
        if (!active_) return;
        if (!finishing_ && now >= nextRound_) {
            if (now >= nextRoute_) {
                const auto route = transport_->route();
                if (route != lastRoute_ || nextRoute_ == 0) {
                    lastRoute_ = route;
                    auto settings = actual_;
                    settings.insert(route);
                    emit owner_->measurement(id_, QDateTime::currentMSecsSinceEpoch(), settings);
                    if (!active_) return;
                }
                nextRoute_ = now + 5000;
            }
            auto &r = rounds_[++round_];
            r.stamp = QDateTime::currentMSecsSinceEpoch();
            r.limit = limit_;
            nextRound_ = now + interval_;
            send(round_, r, 0);
            if (!active_) return;
        }
        auto wake = finishing_ ? now + responseTimeoutMs : nextRound_;
        for (auto &[number, r] : rounds_) {
            if (r.nextHop <= r.limit && now >= r.nextSend) {
                for (int i = 0; i < burst && r.nextHop <= r.limit; ++i) {
                    send(number, r, r.nextHop++);
                    if (!active_) return;
                }
                r.nextSend = now + burstSpacingMs;
            }
            if (r.nextHop <= r.limit) wake = std::min(wake, r.nextSend);
        }
        flush();
        insideTick_ = false;
        if (!active_) return;
        if (finishing_ && rounds_.empty()) {
            cancel();
            emit owner_->finished(id_);
            return;
        }
        if (!pending_.empty()) wake = std::min(wake, pending_.begin()->second.deadline);
        timer_.start(int(std::max<qint64>(1, wake - clock_.elapsed())));
    }

    Probe *owner_;
    qint64 id_;
    ProbeOptions options_;
    int interval_, lookup_ = -1, limit_ = maxHops;
    bool active_ = true, finishing_ = false, insideTick_ = false;
    quint64 round_ = 0, token_ = 0;
    qint64 nextRound_ = 0, nextRoute_ = 0;
    QTimer timer_;
    QElapsedTimer clock_;
    QVariantMap actual_, lastRoute_;
    std::map<quint64, Round> rounds_;
    std::map<quint64, Pending> pending_;
    std::unique_ptr<NativeEcho> transport_;
};
} // namespace

ProbeSession *newProbeSession(Probe *owner, qint64 id, int intervalMs, ProbeOptions options) {
    return new EchoSession(owner, id, intervalMs, std::move(options));
}
