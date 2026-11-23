#include "history.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <sqlite3.h>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr std::array<qint64, 3> spans{10000, 60000, 3600000};
constexpr qint64 timeLimit = std::numeric_limits<qint64>::max() - 3600001;

void check(sqlite3 *db, int result) {
    if (result != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void execute(sqlite3 *db, const char *sql) {
    check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class Statement {
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        check(db, sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void reset() { check(db_, sqlite3_reset(stmt_)); }
    void integer(int i, qint64 n) { check(db_, sqlite3_bind_int64(stmt_, i, n)); }
    void number(int i, double n) { check(db_, sqlite3_bind_double(stmt_, i, n)); }
    void string(int i, const QString &s) {
        const auto u = s.toUtf8();
        check(db_, sqlite3_bind_text(stmt_, i, u.constData(), u.size(), SQLITE_TRANSIENT));
    }
    bool next() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW)
            return true;
        if (result != SQLITE_DONE)
            check(db_, result);
        return false;
    }
    qint64 integer(int i) const { return sqlite3_column_int64(stmt_, i); }
    double number(int i) const { return sqlite3_column_double(stmt_, i); }
    QString string(int i) const {
        return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt_, i)));
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *stmt_ = nullptr;
};

class Transaction {
  public:
    Transaction(sqlite3 *db, bool writer) : db_(db) { execute(db_, writer ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() {
        if (active_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execute(db_, "COMMIT");
        active_ = false;
    }

  private:
    sqlite3 *db_;
    bool active_ = true;
};

// Rollups retain the first and last successful RTT so joining adjacent buckets
// includes the jitter transition across the boundary. A loss counts as a sent
// probe and as a missing current RTT, never as a zero-latency measurement.
struct Stats {
    qint64 sent = 0, received = 0;
    qint64 firstMs = 0, lastMs = 0;
    double low = -1, high = -1, sum = 0, firstRtt = -1, lastRtt = -1, current = -1, delta = 0;
    QString firstAddress, address;
    bool routeChanged = false;
    bool destination = false;

    void add(const Stats &after) {
        if (!after.sent)
            return;
        if (!sent) {
            *this = after;
            return;
        }
        if (after.received) {
            if (received) {
                delta += std::abs(lastRtt - after.firstRtt);
                low = std::min(low, after.low);
            } else {
                low = after.low;
                firstRtt = after.firstRtt;
            }
            high = std::max(high, after.high);
            sum += after.sum;
            delta += after.delta;
            lastRtt = after.lastRtt;
        }
        sent += after.sent;
        received += after.received;
        current = after.current;
        lastMs = after.lastMs;
        if (!after.address.isEmpty()) {
            routeChanged |= after.routeChanged || (!address.isEmpty() && address != after.firstAddress);
            if (firstAddress.isEmpty())
                firstAddress = after.firstAddress;
            address = after.address;
        }
        destination |= after.destination;
    }

    static Stats sample(qint64 t, double r, const QString &addr, bool dest) {
        Stats s;
        s.sent = 1;
        s.firstMs = s.lastMs = t;
        s.firstAddress = s.address = addr;
        s.destination = dest;
        if (r >= 0) {
            s.received = 1;
            s.low = s.high = s.sum = s.firstRtt = s.lastRtt = s.current = r;
        }
        return s;
    }
    double loss() const { return sent ? 100.0 * (sent - received) / sent : 0; }
    double average() const { return received ? sum / received : -1; }
};

constexpr char schema[] = R"SQL(
CREATE TABLE IF NOT EXISTS sessions(
 id INTEGER PRIMARY KEY,target TEXT NOT NULL,family INTEGER NOT NULL,interval_ms INTEGER NOT NULL,
 created_ms INTEGER NOT NULL,first_ms INTEGER,last_ms INTEGER);
CREATE TABLE IF NOT EXISTS notes(
 session INTEGER NOT NULL,t INTEGER NOT NULL,body TEXT NOT NULL,PRIMARY KEY(session,t),
 FOREIGN KEY(session) REFERENCES sessions(id)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS hops(
 session INTEGER NOT NULL,hop INTEGER NOT NULL,first_ms INTEGER NOT NULL,last_ms INTEGER NOT NULL,
 PRIMARY KEY(session,hop),FOREIGN KEY(session) REFERENCES sessions(id)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS samples(
 session INTEGER NOT NULL,hop INTEGER NOT NULL,t INTEGER NOT NULL,rtt REAL NOT NULL,
 address TEXT NOT NULL,dest INTEGER NOT NULL,PRIMARY KEY(session,hop,t),
 FOREIGN KEY(session) REFERENCES sessions(id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS sample_destination ON samples(session,t DESC) WHERE dest=1;
CREATE TABLE IF NOT EXISTS bins(
 session INTEGER NOT NULL,hop INTEGER NOT NULL,span INTEGER NOT NULL,t INTEGER NOT NULL,
 sent INTEGER NOT NULL,received INTEGER NOT NULL,first_ms INTEGER NOT NULL,last_ms INTEGER NOT NULL,
 lo REAL NOT NULL,hi REAL NOT NULL,total REAL NOT NULL,first_rtt REAL NOT NULL,last_rtt REAL NOT NULL,
 current REAL NOT NULL,delta REAL NOT NULL,address TEXT NOT NULL,dest INTEGER NOT NULL,
 first_address TEXT NOT NULL, route_changed INTEGER NOT NULL,
 PRIMARY KEY(session,hop,span,t)) WITHOUT ROWID;
)SQL";

Stats readBin(const Statement &q) {
    Stats s;
    s.sent = q.integer(0);
    s.received = q.integer(1);
    s.firstMs = q.integer(2);
    s.lastMs = q.integer(3);
    s.low = q.number(4);
    s.high = q.number(5);
    s.sum = q.number(6);
    s.firstRtt = q.number(7);
    s.lastRtt = q.number(8);
    s.current = q.number(9);
    s.delta = q.number(10);
    s.address = q.string(11);
    s.destination = q.integer(12);
    s.firstAddress = q.string(13);
    s.routeChanged = q.integer(14);
    return s;
}

constexpr char binsSql[] =
    "SELECT sent,received,first_ms,last_ms,lo,hi,total,first_rtt,"
    "last_rtt,current,delta,address,dest,first_address,route_changed FROM bins WHERE session=?1 AND hop=?2 "
    "AND span=?3 AND t>=?4 AND t<=?5 ORDER BY t";
constexpr char rawSql[] = "SELECT t,rtt,address,dest FROM samples WHERE session=?1 "
                          "AND hop=?2 AND t>=?3 AND t<=?4 ORDER BY t";

struct Reader {
    explicit Reader(sqlite3 *db) : bins(db, binsSql), raw(db, rawSql) {}
    Statement bins, raw;

    // Each requested range is exactly covered by whole rollups and the raw
    // samples at its two edges. Complexity depends on pixels, not run length.
    void range(Stats &out, qint64 session, int hop, qint64 from, qint64 to, int level = spans.size() - 1) {
        if (from > to)
            return;
        if (level < 0) {
            raw.reset();
            raw.integer(1, session);
            raw.integer(2, hop);
            raw.integer(3, from);
            raw.integer(4, to);
            while (raw.next())
                out.add(Stats::sample(raw.integer(0), raw.number(1), raw.string(2), raw.integer(3)));
            return;
        }
        const qint64 span = spans[level];
        const qint64 first = from + (span - from % span) % span;
        // 'last' is the exclusive end of the last complete bucket.
        const qint64 last = (to + 1) / span * span;
        if (first >= last) {
            range(out, session, hop, from, to, level - 1);
            return;
        }
        range(out, session, hop, from, first - 1, level - 1);
        bins.reset();
        bins.integer(1, session);
        bins.integer(2, hop);
        bins.integer(3, span);
        bins.integer(4, first);
        bins.integer(5, last - 1);
        while (bins.next())
            out.add(readBin(bins));
        range(out, session, hop, last, to, level - 1);
    }
};

void saveBin(Statement &q, qint64 session, int hop, qint64 span, qint64 start, const Stats &s) {
    q.reset();
    q.integer(1, session);
    q.integer(2, hop);
    q.integer(3, span);
    q.integer(4, start);
    q.integer(5, s.sent);
    q.integer(6, s.received);
    q.integer(7, s.firstMs);
    q.integer(8, s.lastMs);
    q.number(9, s.low);
    q.number(10, s.high);
    q.number(11, s.sum);
    q.number(12, s.firstRtt);
    q.number(13, s.lastRtt);
    q.number(14, s.current);
    q.number(15, s.delta);
    q.string(16, s.address);
    q.integer(17, s.destination);
    q.string(18, s.firstAddress);
    q.integer(19, s.routeChanged);
    q.next();
}
constexpr char saveBinSql[] = "INSERT OR REPLACE INTO bins VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

// Upgrade older rollups using the original observations. The
// cache and the new destination series are committed together by open().
void upgradeRollups(sqlite3 *db) {
    Statement hasColumns(db, "SELECT count(*) FROM pragma_table_info('bins') WHERE name='first_address'");
    if (hasColumns.next() && hasColumns.integer(0))
        return;
    hasColumns.reset();
    execute(db, "ALTER TABLE bins ADD COLUMN first_address TEXT NOT NULL DEFAULT '';"
                "ALTER TABLE bins ADD COLUMN route_changed INTEGER NOT NULL DEFAULT 0;"
                "DELETE FROM bins;"
                "INSERT INTO samples(session,hop,t,rtt,address,dest) "
                "SELECT session,0,t,coalesce(max(CASE WHEN dest THEN rtt END),-1),"
                "coalesce(max(CASE WHEN dest THEN address END),''),0 "
                "FROM samples WHERE hop>0 GROUP BY session,t");
    Statement raw(db, "SELECT session,hop,t,rtt,address,dest FROM samples ORDER BY session,hop,t");
    Statement put(db, saveBinSql);
    std::array<Stats, spans.size()> totals;
    qint64 prevId = 0;
    int prevHop = 0;
    auto flush = [&](size_t level) {
        auto &s = totals[level];
        if (s.sent)
            saveBin(put, prevId, prevHop, spans[level], s.firstMs / spans[level] * spans[level], s);
        s = {};
    };
    while (raw.next()) {
        const qint64 id = raw.integer(0), t = raw.integer(2);
        const int hop = raw.integer(1);
        const auto one = Stats::sample(t, raw.number(3), raw.string(4), raw.integer(5));
        for (size_t i = 0; i < spans.size(); ++i) {
            if (prevId != id || prevHop != hop || totals[i].firstMs / spans[i] != t / spans[i])
                flush(i);
            totals[i].add(one);
        }
        prevId = id;
        prevHop = hop;
    }
    for (size_t i = 0; i < spans.size(); ++i)
        flush(i);
}
} // namespace

History::History(QString path) : path_(std::move(path)) {
}
History::~History() {
    if (db_)
        sqlite3_close(db_);
}
QString History::error() const {
    return error_;
}

bool History::open() {
    if (db_)
        return true;
    error_.clear();
    if (path_ != ":memory:" && !QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error_ = QStringLiteral("Cannot create history directory");
        return false;
    }
    try {
        const auto p = QFile::encodeName(path_);
        const int rc =
            sqlite3_open_v2(p.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK)
            throw std::runtime_error(db_ ? sqlite3_errmsg(db_) : "Cannot open SQLite");
        sqlite3_busy_timeout(db_, 5000);
        execute(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON");
        Transaction tx(db_, true);
        execute(db_, schema);
        upgradeRollups(db_);
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
}

qint64 History::createSession(QString target, int family, int intervalMs) {
    if (!open())
        return 0;
    error_.clear();
    try {
        Statement q(db_, "INSERT INTO sessions(target,family,interval_ms,created_ms) VALUES(?,?,?,?)");
        q.string(1, target);
        q.integer(2, family);
        q.integer(3, intervalMs);
        q.integer(4, QDateTime::currentMSecsSinceEpoch());
        q.next();
        return sqlite3_last_insert_rowid(db_);
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return 0;
    }
}

bool History::setInterval(qint64 id, int intervalMs) {
    if (!open())
        return false;
    error_.clear();
    try {
        if (intervalMs <= 0)
            throw std::runtime_error("Invalid probe interval");
        Statement q(db_, "UPDATE sessions SET interval_ms=? WHERE id=?");
        q.integer(1, intervalMs);
        q.integer(2, id);
        q.next();
        if (sqlite3_changes(db_) == 0)
            throw std::runtime_error("Unknown session");
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::addNote(qint64 id, qint64 timeMs, QString text) {
    if (!open())
        return false;
    error_.clear();
    try {
        const bool remove = text.trimmed().isEmpty();
        Statement q(
            db_,
            remove ? "DELETE FROM notes WHERE session=? AND t=?"
                   : "INSERT INTO notes VALUES(?,?,?) ON CONFLICT(session,t) DO UPDATE SET body=excluded.body");
        q.integer(1, id);
        q.integer(2, timeMs);
        if (!remove)
            q.string(3, text);
        q.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantList History::sessions() {
    QVariantList list;
    if (!open())
        return list;
    error_.clear();
    try {
        Statement q(db_, "SELECT id,target,family,interval_ms,coalesce(first_ms,created_ms),"
                         "coalesce(last_ms,created_ms) FROM sessions ORDER BY id DESC");
        while (q.next())
            list << QVariantMap{{"id", q.integer(0)},      {"target", q.string(1)},
                                {"family", q.integer(2)},  {"intervalMs", q.integer(3)},
                                {"startMs", q.integer(4)}, {"endMs", q.integer(5)}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        list.clear();
    }
    return list;
}

bool History::append(const QVector<Sample> &samples) {
    if (!open())
        return false;
    error_.clear();
    if (samples.isEmpty())
        return true;
    try {
        Transaction tx(db_, true);
        Statement insert(db_, "INSERT INTO samples VALUES(?,?,?,?,?,?) ON CONFLICT(session,hop,t) DO NOTHING");
        Statement setDestination(
            db_,
            "INSERT INTO samples VALUES(?,0,?,?,?,0) ON CONFLICT(session,hop,t) "
            "DO UPDATE SET rtt=excluded.rtt,address=excluded.address WHERE samples.rtt<0 AND excluded.rtt>=0");
        std::map<std::pair<qint64, qint64>, Stats> rounds;
        using Key = std::tuple<qint64, int, qint64, qint64>; // session, hop, span, bucket start
        std::map<Key, Stats> changed;
        std::map<std::pair<qint64, int>, std::pair<qint64, qint64>> bounds;
        std::vector<const Sample *> ordered;
        ordered.reserve(samples.size());
        for (const auto &s : samples)
            ordered.push_back(&s);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->timeMs < b->timeMs; });
        for (const auto *s : ordered) {
            if (s->timeMs < 0 || s->timeMs > timeLimit || s->hop < 1 || s->hop > 255 ||
                !std::isfinite(s->rttMs))
                throw std::runtime_error("Invalid probe sample");
            insert.reset();
            insert.integer(1, s->session);
            insert.integer(2, s->hop);
            insert.integer(3, s->timeMs);
            insert.number(4, s->rttMs);
            insert.string(5, s->address);
            insert.integer(6, s->destination);
            insert.next();
            if (!sqlite3_changes(db_))
                continue;
            const Stats one = Stats::sample(s->timeMs, s->rttMs, s->address, s->destination);
            // The prober timestamps all TTLs in a round together. Hop zero
            // records exactly one outcome for the target, across all route lengths.
            auto &round = rounds[{s->session, s->timeMs}];
            if (!round.sent || s->destination)
                round = Stats::sample(s->timeMs, s->destination ? s->rttMs : -1,
                                      s->destination ? s->address : QString(), false);
            for (const auto span : spans)
                changed[{s->session, s->hop, span, s->timeMs / span * span}].add(one);
            auto [it, added] = bounds.try_emplace({s->session, s->hop}, s->timeMs, s->timeMs);
            if (!added)
                it->second.second = s->timeMs;
        }

        for (const auto &[key, outcome] : rounds) {
            const auto [id, time] = key;
            setDestination.reset();
            setDestination.integer(1, id);
            setDestination.integer(2, time);
            setDestination.number(3, outcome.current);
            setDestination.string(4, outcome.address);
            setDestination.next();
            if (sqlite3_changes(db_))
                for (const auto span : spans)
                    changed[{id, 0, span, time / span * span}].add(outcome);
        }
        Reader reader(db_);
        Statement put(db_, saveBinSql);
        for (auto &[key, delta] : changed) {
            const auto [id, hop, span, t] = key;
            auto &get = reader.bins;
            get.reset();
            get.integer(1, id);
            get.integer(2, hop);
            get.integer(3, span);
            get.integer(4, t);
            get.integer(5, t);
            Stats all;
            if (get.next())
                all = readBin(get);
            get.reset();
            if (all.sent && delta.firstMs <= all.lastMs) {
                // A late probe can change two jitter transitions. Smaller time
                // buckets are already current because smaller spans sort first.
                all = {};
                const int level = int(std::find(spans.begin(), spans.end(), span) - spans.begin());
                reader.range(all, id, hop, t, t + span - 1, level - 1);
            } else {
                all.add(delta);
            }
            saveBin(put, id, hop, span, t, all);
        }

        Statement hop(db_, "INSERT INTO hops VALUES(?,?,?,?) ON CONFLICT(session,hop) DO UPDATE SET "
                           "first_ms=min(first_ms,excluded.first_ms),last_ms=max(last_ms,excluded.last_ms)");
        Statement session(db_, "UPDATE sessions SET first_ms=min(coalesce(first_ms,?1),?1),"
                               "last_ms=max(coalesce(last_ms,?2),?2) WHERE id=?3");
        for (auto &[key, edge] : bounds) {
            hop.reset();
            hop.integer(1, key.first);
            hop.integer(2, key.second);
            hop.integer(3, edge.first);
            hop.integer(4, edge.second);
            hop.next();
            session.reset();
            session.integer(1, edge.first);
            session.integer(2, edge.second);
            session.integer(3, key.first);
            session.next();
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantMap History::view(qint64 id, qint64 start, qint64 end, qint64 focusStart, qint64 focusEnd,
                          QList<int> selected, int pixels) {
    pixels = std::clamp(pixels, 1, 8192);
    QVariantMap result{{"rows", QVariantList{}},
                       {"charts", QVariantList{}},
                       {"pixels", pixels},
                       {"timeMax", 0.0},
                       {"notes", QVariantList{}}};
    if (!open())
        return result;
    error_.clear();
    // Small guard band at the integer limit keeps aligned bucket arithmetic safe.
    start = std::clamp(start, 0LL, timeLimit);
    end = std::clamp(end, start, timeLimit);
    focusStart = std::clamp(focusStart, 0LL, timeLimit);
    focusEnd = std::clamp(focusEnd, focusStart, timeLimit);
    try {
        Transaction tx(db_, false);
        Reader reader(db_);
        Statement hops(db_, "SELECT hop FROM hops WHERE session=? AND first_ms<=? AND last_ms>=? ORDER BY hop");
        hops.integer(1, id);
        hops.integer(2, focusEnd);
        hops.integer(3, focusStart);
        QVariantList rows, charts;
        double timeMax = 0;
        while (hops.next()) {
            const int hop = hops.integer(0);
            Stats s;
            reader.range(s, id, hop, focusStart, focusEnd);
            if (!s.sent)
                continue;
            rows << QVariantMap{{"hop", hop},
                                {"address", s.address},
                                {"firstAddress", s.firstAddress},
                                {"routeChanged", s.routeChanged},
                                {"sent", s.sent},
                                {"loss", s.loss()},
                                {"min", s.low},
                                {"avg", s.average()},
                                {"max", s.high},
                                {"cur", s.current},
                                {"jitter", s.received > 1 ? s.delta / (s.received - 1) : -1},
                                {"destination", s.destination}};
        }

        int destinationHop = 0;
        if (selected.contains(0)) {
            Statement dest(
                db_, "SELECT hop FROM samples WHERE session=? AND dest=1 AND t<=? ORDER BY t DESC LIMIT 1");
            dest.integer(1, id);
            dest.integer(2, end);
            if (dest.next())
                destinationHop = dest.integer(0);
            else {
                Statement q(db_, "SELECT max(hop) FROM hops WHERE session=? AND first_ms<=? AND last_ms>=?");
                q.integer(1, id);
                q.integer(2, end);
                q.integer(3, start);
                if (q.next())
                    destinationHop = q.integer(0);
            }
            if (destinationHop > 0)
                selected.removeAll(destinationHop);
        }
        QSet<int> seen;
        const qint64 duration = end - start + 1;
        const int count = static_cast<int>(std::min<qint64>(pixels, duration));
        for (const auto hop : selected) {
            if (hop < 0 || (hop == 0 && destinationHop <= 0) || seen.contains(hop))
                continue;
            seen.insert(hop);
            QVector<Stats> points(count);
            if (duration <= qint64(count) * 1000) {
                // Live views often have only a few hundred probes. One index
                // scan is cheaper than an SQLite query for each empty pixel.
                auto &raw = reader.raw;
                raw.reset();
                raw.integer(1, id);
                raw.integer(2, hop);
                raw.integer(3, start);
                raw.integer(4, end);
                while (raw.next()) {
                    const auto t = raw.integer(0);
                    const auto x = (t - start) * count / duration;
                    points[x].add(Stats::sample(t, raw.number(1), raw.string(2), raw.integer(3)));
                }
            } else {
                // ceil(i*duration/count), without overflowing long recording times.
                auto edge = [&](qint64 p) {
                    return start + p * (duration / count) + (p * (duration % count) + count - 1) / count;
                };
                for (int i = 0; i < count; ++i)
                    reader.range(points[i], id, hop, edge(i), edge(i + 1) - 1);
            }
            QVariantList buckets;
            for (int i = 0; i < count; ++i) {
                const Stats &s = points[i];
                timeMax = std::max(timeMax, s.high);
                if (s.sent)
                    buckets << QVariantMap{{"x", int(qint64(i) * pixels / count)},
                                           {"min", s.low},
                                           {"max", s.high},
                                           {"avg", s.average()},
                                           {"loss", s.loss()},
                                           {"sent", s.sent}};
            }
            charts << QVariantMap{
                {"hop", hop == 0 ? destinationHop : hop}, {"destination", hop == 0}, {"buckets", buckets}};
        }
        Statement note(db_, "SELECT t,body FROM notes WHERE session=? AND t>=? AND t<=? ORDER BY t");
        note.integer(1, id);
        note.integer(2, start);
        note.integer(3, end);
        QVariantList notes;
        while (note.next())
            notes << QVariantMap{{"timeMs", note.integer(0)}, {"text", note.string(1)}};
        tx.commit();
        result["notes"] = notes;
        result["rows"] = rows;
        result["charts"] = charts;
        result["timeMax"] = timeMax;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return result;
}
