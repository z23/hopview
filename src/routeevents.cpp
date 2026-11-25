#include "routeevents.h"

#include <QSet>
#include <algorithm>

QVariantMap RouteEvents::observe(const RouteRound &round) {
    constexpr int confirmation = 3;
    if (!recent_.empty()) {
        const auto gap = round.timeMs - recent_.back().stamp;
        if (gap > std::max(5000LL, 4 * previousIntervalMs_)) {
            for (auto &hop : hops_) {
                hop.streak = 0;
                hop.recentRuns.clear();
            }
            destinationStreak_ = 0;
            recent_.clear();
        }
        previousIntervalMs_ = std::clamp(gap, 250LL, 60000LL);
    }
    recent_.push_back({round.timeMs, round.destinationRtt});
    if (recent_.size() > confirmation * 2)
        recent_.pop_front();

    if (!round.replies.isEmpty() && round.replies.lastKey() >= hops_.size())
        hops_.resize(round.replies.lastKey() + 1);

    // Save only changed/replaced addresses. Stable rounds allocate no path snapshots.
    QMap<int, QString> previous;
    QSet<int> changed, returning;
    const int oldDestination = destination_;
    const QString oldTargetAddress = targetAddress_;
    if (round.destinationHop == destinationCandidate_ && round.destinationHop > 0)
        destinationStreak_ = std::min(confirmation, destinationStreak_ + 1);
    else {
        destinationCandidate_ = round.destinationHop;
        destinationStreak_ = round.destinationHop ? 1 : 0;
    }
    if (destinationStreak_ == confirmation) {
        destination_ = destinationCandidate_;
        targetAddress_ = round.replies.value(destination_);
    }

    for (int ttl = 1; ttl < hops_.size(); ++ttl) {
        auto &hop = hops_[ttl];
        const auto reply = round.replies.value(ttl);
        if (!reply.isEmpty() && (hop.recentRuns.isEmpty() || hop.recentRuns.last() != reply)) {
            hop.recentRuns.append(reply);
            if (hop.recentRuns.size() > 32) hop.recentRuns.removeFirst();
        }
        if (reply.isEmpty()) {
            hop.streak = 0;
        } else if (hop.candidate != reply || !hop.streak) {
            hop.candidate = reply;
            hop.streak = 1;
        } else if (hop.streak < confirmation) {
            if (++hop.streak == confirmation && hop.address != reply) {
                previous.insert(ttl, hop.address);
                if (hop.previous.contains(reply) || hop.recentRuns.count(reply) >= 3) returning.insert(ttl);
                if (!hop.address.isEmpty()) {
                    hop.previous.removeAll(hop.address);
                    hop.previous.prepend(hop.address);
                    if (hop.previous.size() > 16) hop.previous.removeLast();
                }
                if (!hop.address.isEmpty() && (!destination_ || ttl <= destination_))
                    changed.insert(ttl);
                hop.address = reply;
            }
        }
    }

    if (destination_ && oldDestination != destination_) {
        // With a longer path, an unanswered former destination TTL is unknown.
        // On a shorter path, discard the old suffix so it cannot reappear later.
        if (oldDestination && destination_ > oldDestination && !previous.contains(oldDestination) &&
            hops_[oldDestination].address == oldTargetAddress) {
            previous.insert(oldDestination, hops_[oldDestination].address);
            hops_[oldDestination] = {};
        }
        if (oldDestination) {
            for (int ttl = std::min(oldDestination, destination_); ttl <= std::max(oldDestination, destination_); ++ttl)
                changed.insert(ttl);
        }
        for (int ttl = destination_ + 1; ttl < hops_.size(); ++ttl) {
            if (!previous.contains(ttl))
                previous.insert(ttl, hops_[ttl].address);
            hops_[ttl] = {};
        }
    }
    if (changed.isEmpty())
        return {};

    const int oldExtent = oldDestination ? oldDestination : int(hops_.size()) - 1;
    const int newExtent = destination_ ? destination_ : int(hops_.size()) - 1;
    QVariantList path;
    QStringList numbers;
    for (int ttl = 1; ttl <= std::max(oldExtent, newExtent); ++ttl) {
        const auto &current = hops_[ttl].address;
        const QString before = previous.value(ttl, current);
        path << QVariantMap{{"hop", ttl}, {"before", before}, {"after", current},
                            {"inBefore", ttl <= oldExtent}, {"inAfter", ttl <= newExtent},
                            {"wasTarget", ttl == oldDestination && before == oldTargetAddress},
                            {"isTarget", ttl == destination_ && current == targetAddress_},
                            {"knownAlternate", returning.contains(ttl)}, {"changed", changed.contains(ttl)}};
        if (changed.contains(ttl))
            numbers << QString::number(ttl);
    }

    const auto start = recent_.size() - confirmation;
    double beforeSum = 0, afterSum = 0;
    int beforeN = 0, afterN = 0;
    for (size_t i = 0; i < recent_.size(); ++i) {
        if (recent_[i].rtt < 0)
            continue;
        if (i < start) {
            beforeSum += recent_[i].rtt;
            ++beforeN;
        } else {
            afterSum += recent_[i].rtt;
            ++afterN;
        }
    }
    return {{"firstMs", recent_[start].stamp}, {"confirmedMs", round.timeMs},
            {"startMs", recent_.front().stamp}, {"knownAlternate", !returning.isEmpty() && returning == changed && oldDestination == destination_}, {"changedHops", numbers.join(", ")}, {"path", path},
            {"beforeRtt", beforeN ? beforeSum / beforeN : -1}, {"afterRtt", afterN ? afterSum / afterN : -1},
            {"beforeN", beforeN}, {"afterN", afterN}};
}

QVariantMap RouteEvents::state() const {
    QVariantList hops, outcomes;
    for (const auto &hop : hops_)
        hops.append(QVariant(QVariantList{hop.address, hop.candidate, hop.streak, hop.previous, hop.recentRuns}));
    for (const auto &outcome : recent_)
        outcomes.append(QVariant(QVariantList{outcome.stamp, outcome.rtt}));
    return {{"hops", hops}, {"recent", outcomes}, {"destination", destination_},
            {"candidate", destinationCandidate_}, {"streak", destinationStreak_},
            {"target", targetAddress_}, {"interval", previousIntervalMs_}};
}

void RouteEvents::restore(const QVariantMap &state) {
    *this = {};
    if (state.isEmpty()) return;
    for (const auto &item : state["hops"].toList()) {
        const auto h = item.toList();
        hops_.append({h.value(0).toString(), h.value(1).toString(), h.value(3).toStringList(), h.value(2).toInt(), h.value(4).toStringList()});
    }
    for (const auto &item : state["recent"].toList()) {
        const auto o = item.toList();
        recent_.push_back({o.value(0).toLongLong(), o.value(1).toDouble()});
    }
    destination_ = state["destination"].toInt();
    destinationCandidate_ = state["candidate"].toInt();
    destinationStreak_ = state["streak"].toInt();
    targetAddress_ = state["target"].toString();
    previousIntervalMs_ = state["interval"].toLongLong();
}
