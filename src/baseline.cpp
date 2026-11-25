#include "history.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <algorithm>
#include <sqlite3.h>

using namespace hopview_sql;
namespace {
struct Period {
    qint64 from, through;
};

// The stored interval is the most recent setting. The synthetic destination series
// has one timestamp per completed probe round, including rounds with no replies.
class RoundClock {
  public:
    RoundClock(sqlite3 *db, qint64 id, qint64 configured)
        : before_(db, "SELECT t FROM samples WHERE session=?1 AND hop=0 AND t<?2 ORDER BY t DESC LIMIT 1"),
          after_(db, "SELECT t FROM samples WHERE session=?1 AND hop=0 AND t>?2 ORDER BY t LIMIT 1"),
          fallback_(std::clamp(configured, 250LL, 60000LL)) {
        before_.integer(1, id);
        after_.integer(1, id);
    }

    bool breaksRun(qint64 previous, qint64 current) {
        const auto elapsed = current - previous;
        if (elapsed <= 3000)
            return false;
        if (elapsed > 3 * 60000)
            return true;
        // Inspect independent spacings on either side. If this hop disappeared
        // while other rounds continued, use spacings INSIDE the missing period.
        const auto prevRound = neighbor(before_, current);
        const auto nextRound = neighbor(after_, previous);
        const bool skippedRounds = nextRound > previous && nextRound < current;
        qint64 left, right;
        if (skippedRounds) {
            left = nextRound - previous;
            right = current - prevRound;
        } else {
            const auto a = neighbor(before_, previous), b = neighbor(after_, current);
            left = a < 0 ? 0 : previous - a;
            right = b < 0 ? 0 : b - current;
        }
        // A pause between recordings is poor evidence of the probe cadence.
        auto plausible = [](qint64 gap) {
            return gap > 0 && gap <= 61000 ? std::clamp(gap, 250LL, 60000LL) : 0LL;
        };
        left = plausible(left);
        right = plausible(right);
        const auto observed = skippedRounds && left && right ? std::min(left, right) : std::max(left, right);
        return elapsed > std::max(3000LL, 3 * (observed ? observed : fallback_));
    }

  private:
    static qint64 neighbor(Statement &query, qint64 time) {
        query.reset();
        query.integer(2, time);
        return query.next() ? query.integer(0) : -1;
    }
    Statement before_, after_;
    qint64 fallback_;
};

QVariantMap measure(sqlite3 *db, qint64 id, int hop, const Period &period, RoundClock &clock) {
    Statement samples(
        db, "SELECT t,rtt,address FROM samples WHERE session=? AND hop=? AND t BETWEEN ? AND ? AND rtt<>-2 ORDER BY t");
    auto bind = [&](Statement &s) {
        s.integer(1, id);
        s.integer(2, hop);
        s.integer(3, period.from);
        s.integer(4, period.through);
    };
    bind(samples);
    qint64 sent = 0, replies = 0, run = 0, longest = 0, previous = -1;
    QSet<QString> responders;
    double delta = 0, precedingRtt = -1;
    while (samples.next()) {
        const auto t = samples.integer(0);

        ++sent;
        if (samples.number(1) >= 0) {
            const auto rtt = samples.number(1);
            if (precedingRtt >= 0)
                delta += std::abs(rtt - precedingRtt);
            precedingRtt = rtt;
            ++replies;
            run = 0;
            responders.insert(samples.string(2));
        } else {
            if (run && clock.breaksRun(previous, t))
                run = 0;
            longest = std::max(longest, ++run);
        }
        previous = t;
    }
    double median = -1, p95 = -1;
    if (replies) {
        // The database can sort on disk. Keep only three order statistics in the client.
        Statement sorted(db, "SELECT rtt FROM samples WHERE session=? AND hop=? AND t BETWEEN ? AND ? "
                             "AND rtt>=0 ORDER BY rtt");
        bind(sorted);
        const auto midLo = (replies - 1) / 2, midHi = replies / 2;
        const auto high = replies - replies / 20 - 1; // ceil(0.95 * count) - 1.
        double lo = 0;
        for (qint64 i = 0; i <= high && sorted.next(); ++i) {
            const auto value = sorted.number(0);
            if (i == midLo)
                lo = value;
            if (i == midHi)
                median = (lo + value) * 0.5;
            if (i == high)
                p95 = value;
        }
    }
    auto addresses = responders.values();
    addresses.sort();
    return {{"sent", sent},          {"received", replies},
            {"median", median},      {"p95", p95},
            {"longest", longest},    {"loss", sent ? 100. * (sent - replies) / sent : -1.},
            {"jitter", replies > 1 ? delta / (replies - 1) : -1.}, {"addresses", addresses}};
}
} // namespace

QVariantList History::comparePeriods(qint64 id, qint64 a, qint64 b, qint64 c, qint64 d,
                                     const std::function<bool()> &cancel) {
    QVariantList rows;
    if (!open())
        return rows;
    error_.clear();
    sqlite3_progress_handler(
        db_, 8192, [](void *ctx) { return (*static_cast<const std::function<bool()> *>(ctx))() ? 1 : 0; },
        const_cast<std::function<bool()> *>(&cancel));
    try {
        Transaction tx(db_, false);
        if (retainedResolution(id,a,b) != "Raw probes" || retainedResolution(id,c,d) != "Raw probes")
            throw std::runtime_error("Exact percentiles and responder sets require raw probes in both intervals.");
        Statement settings(db_, "SELECT interval_ms FROM sessions WHERE id=?");
        settings.integer(1, id);
        if (!settings.next())
            throw std::runtime_error("Recording is unavailable.");
        RoundClock clock(db_, id, settings.integer(0));
        Statement hops(db_, "SELECT hop FROM hops WHERE session=? AND ((first_ms<=? AND last_ms>=?) OR "
                            "(first_ms<=? AND last_ms>=?)) ORDER BY hop");
        hops.integer(1, id);
        hops.integer(2, b);
        hops.integer(3, a);
        hops.integer(4, d);
        hops.integer(5, c);
        QList<int> numbers{0};
        while (hops.next())
            numbers.append(hops.integer(0));
        for (int hop : numbers) {
            if (cancel())
                break;
            const auto base = measure(db_, id, hop, {a, b}, clock);
            const auto incident = measure(db_, id, hop, {c, d}, clock);
            if (hop && !base["sent"].toLongLong() && !incident["sent"].toLongLong())
                continue;
            const bool changed = base["received"].toLongLong() && incident["received"].toLongLong() &&
                                 base["addresses"] != incident["addresses"];
            rows << QVariantMap{
                {"hop", hop}, {"baseline", base}, {"incident", incident}, {"changed", changed}};
        }
        tx.commit();
    } catch (const std::exception &e) {
        rows.clear();
        error_ = QString::fromUtf8(e.what());
    }
    sqlite3_progress_handler(db_, 0, nullptr, nullptr);
    return rows;
}

double History::destinationP95(qint64 id, qint64 from, qint64 through) {
    if (!open()) return -1;
    error_.clear();
    try {
        Transaction tx(db_, false);
        if (retainedResolution(id,from,through) != "Raw probes")
            throw std::runtime_error("Exact p95 requires retained raw probes.");
        RoundClock clock(db_, id, 1000);
        const auto result = measure(db_, id, 0, {from, through}, clock);
        tx.commit();
        return result["p95"].toDouble();
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return -1;
    }
}

QVariantList History::namedBaselines() {
    QVariantList marks;
    if (!open()) return marks;
    try {
        Statement q(db_, "SELECT session,name,from_ms,through_ms,p95,active FROM named_baselines");
        while (q.next())
            marks << QVariantMap{{"session", q.integer(0)}, {"name", q.string(1)}, {"from", q.integer(2)},
                                 {"through", q.integer(3)}, {"p95", q.number(4)}, {"active", bool(q.integer(5))}};
    } catch (const std::exception &e) { error_ = QString::fromUtf8(e.what()); }
    return marks;
}

bool History::storeBaseline(qint64 id, QString name, qint64 from, qint64 through, double p95) {
    if (!open()) return false;
    try {
        if (name.trimmed().isEmpty() || name.size() > 100 || from < 0 || through < from)
            throw std::runtime_error("Invalid baseline name or interval.");
        Transaction tx(db_, true);
        Statement off(db_, "UPDATE named_baselines SET active=0 WHERE session=? AND active=1");
        off.integer(1, id);
        off.next();
        Statement q(db_, "INSERT OR REPLACE INTO named_baselines VALUES(?,?,?,?,?,1)");
        q.integer(1,id); q.string(2,name); q.integer(3,from); q.integer(4,through); q.number(5,p95); q.next();
        tx.commit();
        return true;
    } catch (const std::exception &e) { error_ = QString::fromUtf8(e.what()); return false; }
}

QVariantMap History::destinationPeriods(qint64 id, qint64 from, qint64 through, qint64 baseFrom, qint64 baseEnd) {
    if (!open() || from < 0 || through < from) return {};
    error_.clear();
    try {
        Transaction tx(db_, false);
        Statement interval(db_, "SELECT interval_ms FROM sessions WHERE id=?");
        interval.integer(1, id);
        if (!interval.next()) return {};
        RoundClock clock(db_, id, interval.integer(0));
        if (baseFrom < 0) { baseFrom = std::max(0LL, from - (through - from + 1)); baseEnd = from - 1; }
        const auto stats = [&](qint64 a,qint64 b) {
            return retainedResolution(id,a,b) == "Raw probes" ? measure(db_, id, 0, {a,b}, clock)
                                                              : retainedDestination(id,a,b);
        };
        const auto baseline = stats(baseFrom,baseEnd);
        const auto current = stats(from,through);
        tx.commit();
        return {{"baseline", baseline}, {"incident", current}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

qint64 History::lastDestinationSampleAt(qint64 id, qint64 through) {
    if (!open()) return 0;
    error_.clear();
    try {
        Statement last(db_, "SELECT t FROM samples WHERE session=? AND hop=0 AND t<=? ORDER BY t DESC LIMIT 1");
        last.integer(1, id);
        last.integer(2, through);
        const qint64 raw = last.next() ? last.integer(0) : 0;
        Statement cut(db_, "SELECT raw_before FROM retention WHERE id=1");
        if (!cut.next() || !cut.integer(0) || raw >= cut.integer(0)) return raw;
        Statement archive(db_, "SELECT last_ms FROM bins WHERE session=? AND hop=0 AND span=10000 "
                               "AND t<=? AND last_ms<=? ORDER BY t DESC LIMIT 1");
        archive.integer(1,id); archive.integer(2,std::min(through,cut.integer(0)-1)); archive.integer(3,through);
        return archive.next() ? std::max(raw,archive.integer(0)) : raw;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return 0;
    }
}
