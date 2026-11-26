#include "history.h"
#include "sqlhelpers.h"
#include <algorithm>
#include <cmath>

using namespace hopview_sql;

namespace {
constexpr int highLatency = 1;
constexpr int packetLoss = 2;
constexpr int recovered = 4;
} // namespace

void History::createMonitoringTables() {
    execute(db_, R"SQL(
CREATE TABLE IF NOT EXISTS recorder_settings(
 id INTEGER PRIMARY KEY CHECK(id=1), background INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS recorder_active(
 session INTEGER PRIMARY KEY REFERENCES sessions(id));
CREATE TABLE IF NOT EXISTS alert_rules(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),latency REAL NOT NULL,loss REAL NOT NULL,
 seconds INTEGER NOT NULL,last INTEGER DEFAULT -1,first INTEGER DEFAULT -1,
 sent INTEGER DEFAULT 0,received INTEGER DEFAULT 0,total REAL DEFAULT 0,
 candidate INTEGER DEFAULT 0,candidate_since INTEGER DEFAULT -1,incident_since INTEGER DEFAULT -1);
CREATE TABLE IF NOT EXISTS alert_events(
 id INTEGER PRIMARY KEY, session INTEGER NOT NULL REFERENCES sessions(id),start_ms INTEGER NOT NULL,
 end_ms INTEGER NOT NULL,reason INTEGER NOT NULL,mean_rtt REAL NOT NULL,loss_pct REAL NOT NULL);
CREATE INDEX IF NOT EXISTS alert_events_session_end ON alert_events(session,end_ms);
)SQL");
}

QVariantMap History::monitoringSettings() {
    if (!open()) return {};
    QVariantList active;
    QVariantMap rules;
    error_.clear();
    try {
        Statement pref(db_, "SELECT background FROM recorder_settings WHERE id=1");
        bool background = pref.next() && pref.integer(0);
        Statement sessions(db_, "SELECT session FROM recorder_active EXCEPT SELECT session FROM closed_tabs");
        while (sessions.next())
            active.append(sessions.integer(0));
        Statement q(db_, "SELECT session,latency,loss,seconds FROM alert_rules");
        while (q.next())
            rules[QString::number(q.integer(0))] = QVariantMap{{"latency", q.number(1)}, {"loss", q.number(2)},
                                                               {"seconds", q.integer(3)}};
        return {{"background", background}, {"active", active}, {"rules", rules}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

bool History::saveBackground(bool enabled, const QSet<qint64> &active) {
    if (!open()) return false;
    error_.clear();
    try {
        Transaction tx(db_, true);
        Statement pref(db_, "INSERT OR REPLACE INTO recorder_settings VALUES(1,?)");
        pref.integer(1, enabled);
        pref.next();
        execute(db_, "DELETE FROM recorder_active");
        if (enabled) {
            Statement q(db_, "INSERT INTO recorder_active VALUES(?)");
            for (qint64 id : active) {
                q.reset();
                q.integer(1, id);
                q.next();
            }
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::setAlertRule(qint64 id, double latency, double loss, int seconds) {
    if (!open()) return false;
    error_.clear();
    try {
        if (id <= 0 || !std::isfinite(latency) || !std::isfinite(loss) || latency < 0 || latency > 60000 ||
            loss < 0 || loss > 100 || seconds < 1 || seconds > 86400)
            throw std::runtime_error("Invalid destination alert threshold or window duration");
        if (latency == 0 && loss == 0) {
            Statement q(db_, "DELETE FROM alert_rules WHERE session=?");
            q.integer(1, id);
            q.next();
        } else {
            // Unchanged rules retain their candidate, alarm and recovery state.
            Statement q(db_, "INSERT INTO alert_rules(session,latency,loss,seconds) VALUES(?,?,?,?) "
                             "ON CONFLICT(session) DO UPDATE SET latency=excluded.latency,loss=excluded.loss,"
                             "seconds=excluded.seconds,first=-1,sent=0,received=0,total=0,candidate=0,"
                             "candidate_since=-1 WHERE latency<>excluded.latency OR loss<>excluded.loss "
                             "OR seconds<>excluded.seconds");
            q.integer(1, id);
            q.number(2, latency);
            q.number(3, loss);
            q.integer(4, seconds);
            q.next();
        }
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::resetAlertWindow(qint64 id) {
    if (!open()) return false;
    try {
        Statement q(db_, "UPDATE alert_rules SET first=-1,sent=0,received=0,total=0,candidate=0,"
                         "candidate_since=-1 WHERE session=?");
        q.integer(1, id);
        q.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

std::optional<History::AlertState> History::loadAlertState(qint64 id) {
    Statement q(db_, "SELECT interval_ms,seconds,last,first,sent,received,candidate_since,incident_since,"
                     "latency,loss,total,candidate FROM alert_rules r JOIN sessions s ON s.id=r.session "
                     "WHERE r.session=?");
    q.integer(1, id);
    if (!q.next())
        return {};
    return AlertState{id, q.integer(0), q.integer(1) * 1000, q.integer(2), q.integer(3), q.integer(4),
                      q.integer(5), q.integer(6), q.integer(7), q.number(8), q.number(9), q.number(10),
                      int(q.integer(11))};
}

void History::saveAlertState(const AlertState &s) {
    Statement q(db_, "UPDATE alert_rules SET last=?,first=?,sent=?,received=?,total=?,candidate=?,"
                     "candidate_since=?,incident_since=? WHERE session=?");
    q.integer(1, s.last);
    q.integer(2, s.first);
    q.integer(3, s.sent);
    q.integer(4, s.received);
    q.number(5, s.total);
    q.integer(6, s.candidate);
    q.integer(7, s.candidateSince);
    q.integer(8, s.incidentSince);
    q.integer(9, s.id);
    q.next();
}

void History::evaluateDestination(AlertState &s, qint64 timeMs, double rtt, QList<qint64> &eventIds) {
    // The live prober emits complete rounds. Replayed/late history never reissues an alert.
    if (timeMs <= s.last)
        return;
    if (s.last >= 0 && timeMs - s.last > std::max(3000LL, 3 * s.intervalMs)) {
        s.first = -1;
        s.candidate = 0;
        s.candidateSince = -1;
    }
    s.last = timeMs;
    if (s.first < 0) {
        s.first = timeMs;
        s.sent = s.received = 0;
        s.total = 0;
    }
    ++s.sent;
    if (rtt >= 0) {
        ++s.received;
        s.total += rtt;
    }
    if (timeMs - s.first < s.durationMs)
        return;
    const double mean = s.received ? s.total / s.received : -1;
    const double loss = 100. * (s.sent - s.received) / s.sent;
    const int breached = (s.latency > 0 && mean >= s.latency ? highLatency : 0) |
                         (s.loss > 0 && loss >= s.loss ? packetLoss : 0);
    // Recovery needs positive evidence when latency is the sole configured metric.
    const bool measurable = s.loss > 0 || s.received > 0;
    const bool alarm = s.incidentSince >= 0;
    const int condition = alarm ? (!breached && measurable ? recovered : 0) : breached;
    const int confirmed = s.candidate & condition;
    if (confirmed) {
        Statement event(db_, "INSERT INTO alert_events(session,start_ms,end_ms,reason,mean_rtt,loss_pct) "
                             "VALUES(?,?,?,?,?,?)");
        event.integer(1, s.id);
        event.integer(2, alarm ? s.incidentSince : s.candidateSince);
        event.integer(3, timeMs);
        event.integer(4, confirmed);
        event.number(5, mean);
        event.number(6, loss);
        event.next();
        eventIds.append(sqlite3_last_insert_rowid(db_));
        s.incidentSince = alarm ? -1 : s.candidateSince;
        s.candidate = 0;
        s.candidateSince = -1;
    } else {
        s.candidate = condition;
        s.candidateSince = condition ? s.first : -1;
    }
    s.first = -1;
}

QVariantList History::alertEvents(qint64 afterId) {
    if (!open()) return {};
    QVariantList list;
    error_.clear();
    try {
        const char *fields = "SELECT e.id,session,target,start_ms,end_ms,reason,mean_rtt,loss_pct "
                             "FROM alert_events e JOIN sessions s ON s.id=e.session ";
        QByteArray sql(fields);
        sql += afterId >= 0 ? "WHERE e.id>? ORDER BY e.id DESC" : "ORDER BY e.id DESC LIMIT 200";
        Statement q(db_, sql.constData());
        if (afterId >= 0)
            q.integer(1, afterId);
        while (q.next()) {
            const int reason = q.integer(5);
            const auto label = reason == recovered ? QStringLiteral("Recovered")
                               : reason == (highLatency | packetLoss) ? QStringLiteral("High latency and packet loss")
                               : reason == packetLoss ? QStringLiteral("Packet loss") : QStringLiteral("High latency");
            list << QVariantMap{{"id", q.integer(0)}, {"session", q.integer(1)}, {"target", q.string(2)},
                                {"startMs", q.integer(3)}, {"endMs", q.integer(4)}, {"label", label},
                                {"recovered", reason == recovered}, {"latency", q.number(6)}, {"loss", q.number(7)}};
        }
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        list.clear();
    }
    return list;
}
