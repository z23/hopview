#include "history.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <algorithm>
#include <sqlite3.h>

using namespace hopline_sql;
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
        db, "SELECT t,rtt,address FROM samples WHERE session=? AND hop=? AND t BETWEEN ? AND ? ORDER BY t");
    auto bind = [&](Statement &s) {
        s.integer(1, id);
        s.integer(2, hop);
        s.integer(3, period.from);
        s.integer(4, period.through);
    };
    bind(samples);
    qint64 sent = 0, replies = 0, run = 0, longest = 0, previous = -1;
    QSet<QString> responders;
    while (samples.next()) {
        const auto t = samples.integer(0);

        ++sent;
        if (samples.number(1) >= 0) {
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
        // SQLite may spill this sort to disk. Keep only three order statistics in the client.
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
            {"addresses", addresses}};
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
