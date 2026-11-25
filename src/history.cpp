#include "probeoptions.h"
#include "history.h"
#include "routeevents.h"
#include "sqlhelpers.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

using namespace hopview_sql;

namespace {
constexpr std::array<qint64, 3> spans{10000, 60000, 3600000};
constexpr qint64 timeLimit = std::numeric_limits<qint64>::max() - 3600001;

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
        if (r == -2) return s; // Failed locally, never sent.
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
CREATE TABLE IF NOT EXISTS session_probe(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),protocol TEXT NOT NULL,port INTEGER NOT NULL,
 interface_name TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS session_origin(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),location TEXT NOT NULL,resolved_address TEXT NOT NULL,
 clock_offset_ms INTEGER NOT NULL,clock_error_ms INTEGER NOT NULL,received_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS probe_profiles(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),payload_bytes INTEGER NOT NULL,dscp INTEGER NOT NULL,df INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS measurement_epochs(
 session INTEGER NOT NULL REFERENCES sessions(id), t INTEGER NOT NULL, data TEXT NOT NULL,
 PRIMARY KEY(session,t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS probe_failures(
 session INTEGER NOT NULL REFERENCES sessions(id),hop INTEGER NOT NULL,t INTEGER NOT NULL,data TEXT NOT NULL,
 PRIMARY KEY(session,hop,t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS receiver_streams(
 location TEXT NOT NULL,stream TEXT NOT NULL,session INTEGER NOT NULL UNIQUE REFERENCES sessions(id),
 PRIMARY KEY(location,stream)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS imported_sessions(
 session INTEGER PRIMARY KEY REFERENCES sessions(id), correction_ms INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS route_index(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),cursor INTEGER NOT NULL,state TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS route_checkpoints(
 session INTEGER NOT NULL REFERENCES sessions(id),hour INTEGER NOT NULL,t INTEGER NOT NULL,state TEXT NOT NULL,
 PRIMARY KEY(session,hour)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS route_transitions(
 session INTEGER NOT NULL REFERENCES sessions(id),t INTEGER NOT NULL,data TEXT NOT NULL,
 PRIMARY KEY(session,t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS route_gaps(
 session INTEGER NOT NULL REFERENCES sessions(id),t INTEGER NOT NULL,previous_ms INTEGER NOT NULL,
 PRIMARY KEY(session,t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS inventory_lengths(family INTEGER NOT NULL,bits INTEGER NOT NULL,
 PRIMARY KEY(family,bits)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS network_origins(
 family INTEGER NOT NULL,bits INTEGER NOT NULL,prefix TEXT NOT NULL,data TEXT NOT NULL,
 PRIMARY KEY(family,bits,prefix)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS network_inventory(
 family INTEGER NOT NULL,bits INTEGER NOT NULL,prefix TEXT NOT NULL,data TEXT NOT NULL,
 PRIMARY KEY(family,bits,prefix)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS captured_identity(
 session INTEGER NOT NULL REFERENCES sessions(id),address TEXT NOT NULL,data TEXT NOT NULL,
 PRIMARY KEY(session,address)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS retention(id INTEGER PRIMARY KEY,raw_days INTEGER,summary_days INTEGER,raw_before INTEGER,summary_before INTEGER);
INSERT OR IGNORE INTO retention VALUES(1,0,0,0,0);
CREATE TABLE IF NOT EXISTS retention_pins(session INTEGER REFERENCES sessions(id),start_ms INTEGER,end_ms INTEGER,PRIMARY KEY(session,start_ms,end_ms)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS storage_growth(t INTEGER PRIMARY KEY,bytes INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS named_baselines(
 session INTEGER NOT NULL REFERENCES sessions(id),name TEXT NOT NULL,from_ms INTEGER NOT NULL,
 through_ms INTEGER NOT NULL,p95 REAL NOT NULL,active INTEGER NOT NULL,PRIMARY KEY(session,name)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS comparison_groups(name TEXT PRIMARY KEY, view_json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS comparison_members(
 name TEXT NOT NULL REFERENCES comparison_groups(name) ON DELETE CASCADE,
 position INTEGER NOT NULL, session INTEGER NOT NULL REFERENCES sessions(id),
 PRIMARY KEY(name,position), UNIQUE(name,session)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS closed_tabs(
 session INTEGER PRIMARY KEY REFERENCES sessions(id));
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
    explicit Reader(sqlite3 *db) : bins(db, binsSql), raw(db, rawSql) {
        Statement cut(db,"SELECT raw_before FROM retention WHERE id=1");
        if (cut.next()) rawBefore = cut.integer(0);
        if (rawBefore) {
            archived.emplace(db,"SELECT sent,received,first_ms,last_ms,lo,hi,total,first_rtt,last_rtt,current,delta,address,"
                             "dest,first_address,route_changed,t FROM bins WHERE session=?1 AND hop=?2 AND span=10000 "
                             "AND t BETWEEN ?3 AND ?4 AND session NOT IN (SELECT session FROM imported_sessions) "
                             "AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=?1 AND bins.t BETWEEN p.start_ms AND p.end_ms) ORDER BY t");
        }
    }
    std::optional<Statement> archived;
    qint64 rawBefore = 0;

    // Sub-bin windows plot a retained 10-second aggregate at its midpoint.
    template<class Visit> void summaries(qint64 id,int hop,qint64 from,qint64 to,Visit visit) {
        if (!archived || from >= rawBefore) return;
        archived->reset(); archived->integer(1,id); archived->integer(2,hop);
        archived->integer(3,from-5000); archived->integer(4,std::min(to-5000,rawBefore-10000));
        while (archived->next()) visit(archived->integer(15)+5000,readBin(*archived));
    }
    Statement bins, raw;

    // Each requested range is exactly covered by whole rollups and the raw
    // samples at its two edges. Complexity depends on pixels, not run length.
    void range(Stats &out, qint64 session, int hop, qint64 from, qint64 to, int level = spans.size() - 1) {
        if (from > to)
            return;
        if (level < 0) {
            summaries(session,hop,from,to,[&](qint64,const Stats &s){ out.add(s); });
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

// Compare both halves of the focus to the immediately preceding equal-length
// period. These are patterns across observed TTLs, never a link attribution.
QVariantMap destinationImpact(Reader &reader, qint64 session, qint64 from, qint64 through,
                              const QVariantList &rows) {
    const qint64 mid = from + (through - from) / 2;
    const qint64 before = std::max(0LL, from - (through - from + 1));
    auto halves = [&](int hop) {
        std::array<Stats, 3> s;
        reader.range(s[0], session, hop, before, from - 1);
        reader.range(s[1], session, hop, from, mid);
        reader.range(s[2], session, hop, mid + 1, through);
        return s;
    };
    const auto dst = halves(0);
    const auto rcv = dst[1].received + dst[2].received, sent = dst[1].sent + dst[2].sent;
    const auto hasLoss = [](const auto &s) {
        return s[1].sent >= 3 && s[2].sent >= 3 && s[1].sent > s[1].received && s[2].sent > s[2].received;
    };
    const auto slower = [](const auto &s) {
        if (s[0].received < 3 || s[1].received < 3 || s[2].received < 3 ||
            s[0].routeChanged || s[1].routeChanged || s[2].routeChanged ||
            s[0].address != s[1].address || s[1].address != s[2].address) return false;
        const double limit = s[0].average() + std::max(10., 0.2 * s[0].average());
        return s[1].average() >= limit && s[2].average() >= limit;
    };
    const bool loss = hasLoss(dst), delay = slower(dst);
    QVariantMap result{{"sent", sent}, {"received", rcv}, {"state", "inconclusive"}};
    QString reason;
    if (!sent) reason = "No destination samples in focus.";
    else if (!rcv) reason = "No destination replies. Reachability and the point of failure are uncertain.";
    else if (loss || delay) {
        result["state"] = "affected";
        reason = loss && delay ? "Sustained destination loss and latency increase."
                   : loss ? "Repeated destination reply loss." : "Sustained destination latency increase.";
        int lastDestination = 0, destinations = 0;
        for (const auto &entry : rows) {
            const auto row = entry.toMap();
            if (row["destination"].toBool()) { lastDestination = row["hop"].toInt(); ++destinations; }
        }
        int earliest = 0;
        if (destinations == 1) {
            int expected = lastDestination;
            bool sameLoss = loss, sameDelay = delay;
            for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
                const auto row = it->toMap();
                const int hop = row["hop"].toInt();
                if (hop > lastDestination) continue;
                if (hop != expected-- || row["routeChanged"].toBool()) break;
                const auto s = halves(hop);
                sameLoss = sameLoss && hasLoss(s);
                sameDelay = sameDelay && slower(s);
                if (!sameLoss && !sameDelay) break;
                if (!s[1].received || !s[2].received) break;
                earliest = hop;
            }
        }
        if (earliest && earliest < lastDestination)
            reason += QString(" Downstream pattern from hop %1.").arg(earliest);
        else reason += " Starting hop inconclusive.";
        result["earliestHop"] = earliest && earliest < lastDestination ? earliest : 0;
    } else if (sent == rcv) {
        result["state"] = "answered";
        const auto isolated = std::any_of(rows.begin(), rows.end(), [](const QVariant &v) {
            return v.toMap()["loss"].toDouble() > 0;
        });
        reason = isolated ? "Intermediate reply loss. Every destination round had a reply."
                          : "Every destination round had a reply.";
    } else reason = "Some destination probes timed out. Persistence is inconclusive.";
    result["description"] = reason;
    return result;
}

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
qint64 insertSession(sqlite3 *db, const QString &target, int family, int intervalMs,
                     const QString &protocol, int port, const QString &interfaceName, int payloadBytes, int dscp, bool dontFragment) {
    const ProbeOptions options{protocol, port, interfaceName, payloadBytes, dscp, dontFragment};
    if (!options.valid()) throw std::runtime_error("Invalid probe protocol or destination port");
    Statement q(db, "INSERT INTO sessions(target,family,interval_ms,created_ms) VALUES(?,?,?,?)");
    q.string(1, target);
    q.integer(2, family);
    q.integer(3, intervalMs);
    q.integer(4, QDateTime::currentMSecsSinceEpoch());
    q.next();
    const auto id = sqlite3_last_insert_rowid(db);
    Statement probe(db, "INSERT INTO session_probe(session,protocol,port,interface_name) VALUES(?,?,?,?)");
    probe.integer(1, id);
    probe.string(2, protocol);
    probe.integer(3, options.destinationPort());
    probe.string(4, interfaceName);
    probe.next();
    Statement profile(db,"INSERT INTO probe_profiles VALUES(?,?,?,?)");
    profile.integer(1,id); profile.integer(2,payloadBytes); profile.integer(3,dscp); profile.integer(4,dontFragment);
    profile.next();
    return id;
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

bool History::openReadOnly() {
    if (db_) return sqlite3_db_readonly(db_, "main") == 1;
    if (sqlite3_open_v2(QFile::encodeName(path_).constData(), &db_, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_busy_timeout(db_, 3000);
        return true;
    }
    error_ = QString::fromUtf8(sqlite3_errmsg(db_));
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
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
        createMonitoringTables();
        createDiagnosticTables();
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

qint64 History::createSession(QString target, int family, int intervalMs, QString protocol, int port,
                              QString interfaceName, int payloadBytes, int dscp, bool dontFragment) {
    if (!open())
        return 0;
    error_.clear();
    try {
        Transaction tx(db_, true);
        const auto id = insertSession(db_, target, family, intervalMs, protocol, port, interfaceName, payloadBytes, dscp, dontFragment);
        tx.commit();
        return id;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return 0;
    }
}

qint64 History::receiverSession(const QString &location, const QString &stream, const QString &target,
                                int family, int intervalMs, const QString &protocol, int port,
                                const QString &interfaceName, int payloadBytes, int dscp, bool dontFragment) {
    if (!open()) return 0;
    error_.clear();
    try {
        Transaction tx(db_, true);
        Statement lookup(db_, "SELECT session FROM receiver_streams WHERE location=? AND stream=?");
        lookup.string(1, location);
        lookup.string(2, stream);
        if (lookup.next()) {
            auto id = lookup.integer(0);
            tx.commit();
            return id;
        }
        const auto id = insertSession(db_, target, family, intervalMs, protocol, port, interfaceName, payloadBytes, dscp, dontFragment);
        Statement bind(db_, "INSERT INTO receiver_streams(location,stream,session) VALUES(?,?,?)");
        bind.string(1, location);
        bind.string(2, stream);
        bind.integer(3, id);
        bind.next();
        tx.commit();
        return id;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return 0;
    }
}

bool History::setSessionOrigin(qint64 id, const QString &location, const QString &resolvedAddress,
                               qint64 clockOffsetMs, qint64 clockErrorMs, qint64 lastReceivedMs, qint64 sampleThroughMs) {
    if (!open()) return false;
    error_.clear();
    try {
        Transaction tx(db_, true);
        // Retried or backfilled samples can predate the last clock estimate already visible in this stream.
        if (sampleThroughMs >= 0) {
            Statement old(db_, "UPDATE session_origin SET received_ms=max(received_ms,?1) WHERE session=?2 "
                               "AND (SELECT last_ms FROM sessions WHERE id=?2)>?3");
            old.integer(1, lastReceivedMs);
            old.integer(2, id);
            old.integer(3, sampleThroughMs);
            old.next();
            if (sqlite3_changes(db_)) {
                tx.commit();
                return true;
            }
        }
        Statement bind(db_, "INSERT OR REPLACE INTO session_origin VALUES(?,?,?,?,?,?)");
        bind.integer(1, id);
        bind.string(2, location);
        bind.string(3, resolvedAddress);
        bind.integer(4, clockOffsetMs);
        bind.integer(5, clockErrorMs);
        bind.integer(6, lastReceivedMs);
        bind.next();
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::setMeasurement(qint64 id, qint64 stamp, const QVariantMap &settings) {
    if (!open()) return false;
    try {
        const auto json = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(settings)).toJson(QJsonDocument::Compact));
        Statement q(db_, "INSERT OR REPLACE INTO measurement_epochs(session,t,data) SELECT ?1,?2,?3 WHERE "
                         "coalesce((SELECT data FROM measurement_epochs WHERE session=?1 AND t<=?2 ORDER BY t DESC LIMIT 1),'')<>?3");
        q.integer(1, id); q.integer(2, stamp); q.string(3, json); q.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
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

bool History::setTabClosed(qint64 id, bool closed) {
    if (!open())
        return false;
    error_.clear();
    try {
        Statement q(db_, closed ? "INSERT OR IGNORE INTO closed_tabs(session) VALUES(?)"
                                : "DELETE FROM closed_tabs WHERE session=?");
        q.integer(1, id);
        q.next();
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

QVariantList History::sessions(bool remoteOnly, qint64 onlyId) {
    QVariantList list;
    if (!open())
        return list;
    error_.clear();
    try {
        QByteArray query = "SELECT id,target,family,interval_ms,coalesce(first_ms,created_ms),"
                         "coalesce(last_ms,created_ms),closed_tabs.session IS NOT NULL,"
                         "coalesce(p.protocol,'auto'),coalesce(p.port,33434),coalesce(p.interface_name,''),"
                         "coalesce(o.location,r.location,''),coalesce(o.resolved_address,''),coalesce(o.clock_offset_ms,0),"
                         "coalesce(o.clock_error_ms,-1),coalesce(o.received_ms,0),r.session IS NOT NULL, i.session IS NOT NULL, coalesce(i.correction_ms,0), "
                         "(SELECT data FROM measurement_epochs WHERE session=sessions.id ORDER BY t DESC LIMIT 1),coalesce(profile.payload_bytes,16),coalesce(profile.dscp,0),coalesce(profile.df,0) "
                         "FROM sessions LEFT JOIN session_probe p ON p.session=sessions.id "
                         "LEFT JOIN imported_sessions i ON i.session=sessions.id "
                         "LEFT JOIN receiver_streams r ON r.session=sessions.id LEFT JOIN probe_profiles profile ON profile.session=sessions.id "
                         "LEFT JOIN session_origin o ON o.session=sessions.id "
                         "LEFT JOIN closed_tabs ON closed_tabs.session=sessions.id ";
        if (remoteOnly) query += "WHERE r.session IS NOT NULL ";
        if (onlyId) query += remoteOnly ? "AND sessions.id=? " : "WHERE sessions.id=? ";
        query += "ORDER BY id DESC";
        Statement q(db_, query.constData());
        if (onlyId) q.integer(1, onlyId);
        while (q.next()) {
            auto info = QVariantMap{{"id", q.integer(0)},      {"target", q.string(1)},
                                {"family", q.integer(2)},  {"intervalMs", q.integer(3)},
                                {"startMs", q.integer(4)}, {"endMs", q.integer(5)}, {"closed", bool(q.integer(6))},
                                {"protocol", q.string(7)}, {"port", q.integer(8)}, {"interfaceName", q.string(9)},
                                {"location", q.string(10)}, {"resolvedAddress", q.string(11)}, {"clockOffsetMs", q.integer(12)},
                                {"clockErrorMs", q.integer(13)}, {"lastReceivedMs", q.integer(14)},
                                {"remote", !q.string(10).isEmpty()}, {"remoteLive", bool(q.integer(15))},
                                {"imported", bool(q.integer(16))}, {"manualCorrectionMs", q.integer(17)}, {"payloadBytes", q.integer(19)}, {"dscp", q.integer(20)}, {"dontFragment", bool(q.integer(21))}};
            const auto latest = QJsonDocument::fromJson(q.string(18).toUtf8()).object().toVariantMap();
            for (auto it = latest.cbegin(); it != latest.cend(); ++it) info[it.key()] = it.value();
            list << info;
        }
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        list.clear();
    }
    return list;
}

bool History::append(const QVector<Sample> &samples, QList<qint64> *newEvents) {
    if (newEvents) newEvents->clear();
    if (!open())
        return false;
    error_.clear();
    if (samples.isEmpty())
        return true;
    try {
        Transaction tx(db_, true);
        Statement insert(db_, "INSERT INTO samples VALUES(?,?,?,?,?,?) ON CONFLICT(session,hop,t) DO NOTHING");
        Statement failure(db_, "INSERT INTO probe_failures VALUES(?,?,?,?)");
        Statement setDestination(
            db_,
            "INSERT INTO samples VALUES(?,0,?,?,?,0) ON CONFLICT(session,hop,t) "
            "DO UPDATE SET rtt=excluded.rtt,address=excluded.address WHERE samples.dest=0 AND samples.rtt<0 AND excluded.rtt>=0");
        std::map<std::pair<qint64, qint64>, Stats> rounds;
        std::set<std::pair<qint64, qint64>> measuredRounds;
        using Key = std::tuple<qint64, int, qint64, qint64>; // session, hop, span, bucket start
        std::map<Key, Stats> changed;
        std::map<std::pair<qint64, int>, std::pair<qint64, qint64>> bounds;
        std::vector<const Sample *> ordered;
        ordered.reserve(samples.size());
        for (const auto &s : samples)
            ordered.push_back(&s);
        std::stable_sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->timeMs < b->timeMs; });
        Statement watermark(db_,"SELECT raw_before FROM retention WHERE id=1");
        watermark.next();
        const auto expiredBefore = watermark.integer(0);
        Statement preserved(db_,"SELECT 1 FROM retention_pins WHERE session=? AND ? BETWEEN start_ms AND end_ms");
        for (const auto *s : ordered) {
            if (s->timeMs < expiredBefore) {
                preserved.reset(); preserved.integer(1,s->session); preserved.integer(2,s->timeMs);
                if (!preserved.next()) continue;
            }
            if (s->timeMs < 0 || s->timeMs > timeLimit || s->hop < 0 || s->hop > 255 ||
                !std::isfinite(s->rttMs))
                throw std::runtime_error("Invalid probe sample");
            insert.reset();
            insert.integer(1, s->session);
            insert.integer(2, s->hop);
            insert.integer(3, s->timeMs);
            insert.number(4, s->rttMs);
            insert.string(5, s->address);
            insert.integer(6, s->hop == 0 ? 2 : int(s->destination));
            insert.next();
            if (!sqlite3_changes(db_))
                continue;
            if (!s->failure.kind.isEmpty()) {
                failure.reset();
                failure.integer(1, s->session); failure.integer(2, s->hop); failure.integer(3, s->timeMs);
                failure.string(4, QString::fromUtf8(QJsonDocument(s->failure.json()).toJson(QJsonDocument::Compact)));
                failure.next();
            }
            const Stats one = Stats::sample(s->timeMs, s->rttMs, s->address, s->destination);
            const auto roundKey = std::pair{s->session, s->timeMs};
            auto &round = rounds[roundKey];
            if (s->hop == 0) {
                // The normal-TTL endpoint probe alone determines this round's loss.
                measuredRounds.insert(roundKey);
                round = one;
            } else if (!measuredRounds.contains(roundKey) && (!round.sent || s->destination)) {
                // Older recordings contain only TTL sweeps. Keep their original semantics.
                round = Stats::sample(s->timeMs, s->destination ? s->rttMs : -1,
                                      s->destination ? s->address : QString(), false);
            }
            for (const auto span : spans)
                changed[{s->session, s->hop, span, s->timeMs / span * span}].add(one);
            if (s->hop == 0) continue;
            auto [it, added] = bounds.try_emplace({s->session, s->hop}, s->timeMs, s->timeMs);
            if (!added)
                it->second.second = s->timeMs;
        }

        QList<qint64> eventIds;
        for (const auto &[key, outcome] : rounds) {
            const auto [id, time] = key;
            if (measuredRounds.contains(key)) continue;
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
        for (auto it = rounds.begin(); it != rounds.end();) {
            const auto id = it->first.first;
            auto end = it;
            while (end != rounds.end() && end->first.first == id)
                ++end;
            if (auto rule = loadAlertState(id)) {
                for (; it != end; ++it)
                    if (it->second.sent) evaluateDestination(*rule, it->first.second, it->second.current, eventIds);
                saveAlertState(*rule);
            }
            it = end;
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
            if (all.sent) saveBin(put, id, hop, span, t, all);
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
        QMap<qint64,qint64> earliestNew;
        for (const auto &[key, edge] : bounds)
            earliestNew[key.first] = std::min(earliestNew.value(key.first, edge.first), edge.first);
        for (auto it = earliestNew.cbegin(); it != earliestNew.cend(); ++it)
            updateRouteIndex(it.key(), it.value());
        tx.commit();
        if (newEvents) *newEvents = std::move(eventIds);
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantMap History::retainedDestination(qint64 id, qint64 from, qint64 through) {
    Reader reader(db_);
    Stats stats;
    reader.range(stats, id, 0, from, through);
    return {{"sent",stats.sent}, {"received",stats.received}, {"loss",stats.sent ? stats.loss() : -1.},
            {"p95",-1.}, {"resolution", retainedResolution(id,from,through)}};
}

QVariantMap History::focusedMeasurement(qint64 id, qint64 from, qint64 through) {
    if (!open()) return {};
    try {
        Statement q(db_, "SELECT t,data FROM measurement_epochs WHERE session=?1 AND t BETWEEN "
                         "coalesce((SELECT max(t) FROM measurement_epochs WHERE session=?1 AND t<=?2),?2) AND ?3 ORDER BY t");
        q.integer(1, id);
        q.integer(2, from);
        q.integer(3, through);
        QVariantMap focused;
        bool mixed = false, unknownClock = false;
        QStringList flowModes;
        qint64 firstEpoch = -1, low = std::numeric_limits<qint64>::max(), high = std::numeric_limits<qint64>::min();
        while (q.next()) {
            auto next = QJsonDocument::fromJson(q.string(1).toUtf8()).object().toVariantMap();
            const auto flow = next["flowMode"].toString();
            if (!flow.isEmpty() && !flowModes.contains(flow)) flowModes.append(flow);
            const auto stamp = q.integer(0);
            if (firstEpoch < 0) firstEpoch = stamp;
            else if (!mixed) {
                for (const auto &key : {"resolvedAddress", "effectiveProtocol", "effectivePort",
                                        "effectiveInterface", "flowMode", "sourceAddress"})
                    mixed |= next.value(key) != focused.value(key);
            }
            const auto uncertainty = next.value("clockErrorMs", -1).toLongLong();
            const auto offset = next.value("clockOffsetMs").toLongLong();
            unknownClock |= uncertainty < 0;
            if (uncertainty >= 0) {
                low = std::min(low, offset - uncertainty);
                high = std::max(high, offset + uncertainty);
            }
            focused = std::move(next);
            focused["t"] = stamp;
        }
        if (focused.isEmpty()) return {};
        bool partial = false;
        if (firstEpoch > from) {
            Statement unknown(db_, "SELECT 1 FROM samples WHERE session=? AND hop=0 AND t>=? AND t<? LIMIT 1");
            unknown.integer(1,id);
            unknown.integer(2,from);
            unknown.integer(3,firstEpoch);
            partial = unknown.next();
        }
        const auto offset = focused.value("clockOffsetMs").toLongLong();
        focused["flowModes"] = flowModes;
        focused["partial"] = partial;
        focused["mixed"] = mixed;
        focused["clockErrorMs"] = partial || unknownClock ? -1 : std::max(high-offset,offset-low);
        return focused;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

QVariantMap History::view(qint64 id, qint64 start, qint64 end, qint64 focusStart, qint64 focusEnd,
                          QList<int> selected, int pixels, bool timelinesOnly) {
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
        result["resolution"] = retainedResolution(id,start,end);
        result["measurement"] = focusedMeasurement(id, focusStart, focusEnd);
        Reader reader(db_);
        Statement labels(db_, "SELECT EXISTS(SELECT 1 FROM inventory_lengths) OR "
                              "EXISTS(SELECT 1 FROM captured_identity WHERE session=?)");
        labels.integer(1,id);
        const bool hasLabels = !timelinesOnly && labels.next() && labels.integer(0);
        Statement lastFailure(db_, "SELECT data FROM probe_failures WHERE session=? AND hop=? AND t BETWEEN ? AND ? ORDER BY t DESC LIMIT 1");
        Statement hops(db_, "SELECT hop FROM hops WHERE session=? AND first_ms<=? AND last_ms>=? ORDER BY hop");
        hops.integer(1, id);
        hops.integer(2, focusEnd);
        hops.integer(3, focusStart);
        QVariantList rows, charts;
        double timeMax = 0;
        while (!timelinesOnly && hops.next()) {
            const int hop = hops.integer(0);
            Stats s;
            reader.range(s, id, hop, focusStart, focusEnd);
            lastFailure.reset();
            lastFailure.integer(1,id); lastFailure.integer(2,hop);
            lastFailure.integer(3,focusStart); lastFailure.integer(4,focusEnd);
            const auto failure = lastFailure.next() ? QJsonDocument::fromJson(lastFailure.string(0).toUtf8()).object().toVariantMap() : QVariantMap{};
            if (!s.sent && failure.isEmpty()) continue;
            rows << QVariantMap{{"hop", hop},
                                {"address", s.address},
                                {"firstAddress", s.firstAddress}, {"identity", hasLabels ? networkIdentity(id,s.address) : QVariantMap{}},
                                {"routeChanged", s.routeChanged},
                                {"sent", s.sent},
                                {"loss", s.sent ? s.loss() : -1.}, {"lastFailure", failure},
                                {"min", s.low},
                                {"avg", s.average()},
                                {"max", s.high},
                                {"cur", s.current},
                                {"jitter", s.received > 1 ? s.delta / (s.received - 1) : -1},
                                {"destination", s.destination}};
        }

        if (!timelinesOnly) result["impact"] = destinationImpact(reader, id, focusStart, focusEnd, rows);

        lastFailure.reset();
        lastFailure.integer(1,id); lastFailure.integer(2,0);
        lastFailure.integer(3,focusStart); lastFailure.integer(4,focusEnd);
        if (lastFailure.next()) result["endpointFailure"] = QJsonDocument::fromJson(lastFailure.string(0).toUtf8()).object().toVariantMap();
        Stats endpoint;
        reader.range(endpoint,id,0,focusStart,focusEnd);
        result["endpoint"] = QVariantMap{{"sent",endpoint.sent},{"received",endpoint.received},
            {"loss",endpoint.sent ? endpoint.loss() : -1.},{"avg",endpoint.average()}};
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
            if (hop < 0 || seen.contains(hop))
                continue;
            seen.insert(hop);
            QVector<Stats> points(count);
            if (duration <= qint64(count) * 1000) {
                // Live views often have only a few hundred probes. One index
                // scan is cheaper than a separate database query for each empty pixel.
                reader.summaries(id,hop,start,end,[&](qint64 t,const Stats &s) {
                    points[(t-start)*count/duration].add(s);
                });
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
        if (timelinesOnly && !notes.isEmpty()) {
            if (charts.isEmpty()) charts.append(QVariantMap{{"buckets", QVariantList{}}, {"destination", true}});
            auto chart = charts.first().toMap();
            chart["notes"] = notes;
            charts[0] = chart;
        }
        result["notes"] = notes;
        result["rows"] = rows;
        result["charts"] = charts;
        result["timeMax"] = timeMax;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return result;
}

// Read bounded time slices through the existing (session,hop,t) primary key. The
// synthetic target series supplies the round clock, including complete timeouts.
bool History::routePage(qint64 session, RouteEvents &detector, qint64 &cursor, qint64 &until,
                        QVariantList &events, bool &more) {
    constexpr int pageSize = 512;
    more = false;
    events.clear();
    if (!open())
        return false;
    error_.clear();
    try {
        std::optional<Transaction> tx;
        if (sqlite3_get_autocommit(db_)) tx.emplace(db_, false);
        if (cursor < 0 && until <= 0) {
            Statement end(db_, "SELECT max(t) FROM samples WHERE session=? AND hop=0");
            end.integer(1, session);
            end.next();
            until = end.integer(0);
        }
        Statement clock(db_, "SELECT t FROM samples WHERE session=? AND hop=0 AND t>? AND t<=? ORDER BY t LIMIT ?");
        clock.integer(1, session);
        clock.integer(2, cursor);
        clock.integer(3, until);
        clock.integer(4, pageSize);
        QVector<RouteRound> rounds;
        QHash<qint64, int> index;
        while (clock.next()) {
            const auto stamp = clock.integer(0);
            index.insert(stamp, rounds.size());
            rounds.append({stamp, {}, 0, -1});
        }
        if (!rounds.isEmpty()) {
            const auto start = rounds.front().timeMs, end = rounds.back().timeMs;
            Statement hops(db_, "SELECT hop FROM hops WHERE session=? AND first_ms<=? AND last_ms>=?");
            hops.integer(1, session);
            hops.integer(2, end);
            hops.integer(3, start);
            Statement raw(db_, "SELECT t,address,dest,rtt FROM samples WHERE session=? AND hop=? AND t BETWEEN ? AND ? AND rtt>=0");
            while (hops.next()) {
                const int ttl = hops.integer(0);
                raw.reset();
                raw.integer(1, session);
                raw.integer(2, ttl);
                raw.integer(3, start);
                raw.integer(4, end);
                while (raw.next()) {
                    auto it = index.constFind(raw.integer(0));
                    if (it == index.cend())
                        continue;
                    auto &round = rounds[it.value()];
                    const QString address = raw.string(1);
                    if (!address.isEmpty())
                        round.replies.insert(ttl, address);
                    if (!address.isEmpty() && raw.integer(2) && (!round.destinationHop || ttl < round.destinationHop)) {
                        round.destinationHop = ttl;
                        round.destinationRtt = raw.number(3);
                    }
                }
            }
            for (const auto &round : rounds) {
                auto event = detector.observe(round);
                if (!event.isEmpty())
                    events.append(std::move(event));
            }
            cursor = end;
            more = end < until && rounds.size() == pageSize;
        }
        if (tx) tx->commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}
