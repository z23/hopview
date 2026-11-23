#include "history.h"
#include "sqlhelpers.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

using namespace hopview_sql;
namespace {
constexpr std::array<qint64, 3> binSpans{30000, 300000, 3600000};
constexpr qint64 maximumTime = 253402300799999LL;
bool isString(const QVariantMap &m, const QString &key, qsizetype limit) {
    const auto v = m.value(key);
    return v.typeId() == QMetaType::QString && v.toString().size() <= limit;
}
bool number(const QVariantMap &m, const QString &key, double low, double high) {
    const auto value = m.value(key);
    const double n = value.toDouble();
    return (value.typeId() == QMetaType::Int || value.typeId() == QMetaType::LongLong ||
            value.typeId() == QMetaType::Double) &&
           std::isfinite(n) && n >= low && n <= high;
}
bool integer(const QVariantMap &m, const QString &key, double low, double high) {
    const double n = m.value(key).toDouble();
    return number(m, key, low, high) && std::trunc(n) == n;
}
bool measurement(const QVariantMap &m, const QString &key, double high) {
    return number(m, key, -1, high) && (m.value(key).toDouble() == -1 || m.value(key).toDouble() >= 0);
}
QVariantMap decode(const QString &json) {
    return QJsonDocument::fromJson(json.toUtf8()).object().toVariantMap();
}
bool validUrl(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    return text.size() <= 2048 && url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == "https" || url.scheme() == "http") && url.userInfo().isEmpty() &&
           !url.hasFragment();
}
bool validInterface(const QString &text) {
    return !text.isEmpty() && text.size() <= 15 && !text.contains('/') && !text.contains(QChar::Null) &&
           text != "." && text != "..";
}
bool validRecord(const QVariantMap &record) {
    // Only persisted observation fields are accepted from incident replay.
    static const QStringList common{"kind", "t", "periodMs", "event"};
    static const QStringList service{"url", "address",   "family", "status", "dns",  "connect",
                                     "tls", "firstByte", "ttfb",   "total",  "error"};
    static const QStringList network{"name",    "carrier", "ap",       "rx",      "tx",
                                     "rxDrops", "txDrops", "rxErrors", "txErrors"};
    const auto &specific = record["kind"] == "service" ? service : network;
    for (auto it = record.cbegin(); it != record.cend(); ++it)
        if (!common.contains(it.key()) && !specific.contains(it.key()))
            return false;
    if (!integer(record, "t", 0, maximumTime) || !integer(record, "periodMs", 100, 3600000) ||
        !isString(record, "event", 512))
        return false;
    if (record["kind"] == "service") {
        if (!isString(record, "url", 2048) || !validUrl(record["url"].toString()) ||
            !isString(record, "address", 255) || !integer(record, "status", 0, 999) ||
            (!integer(record, "family", 0, 6) ||
             (record["family"].toInt() != 0 && record["family"].toInt() != 4 &&
              record["family"].toInt() != 6)) ||
            !isString(record, "error", 512))
            return false;
        for (const auto key : {"dns", "connect", "tls", "firstByte", "total"})
            if (!measurement(record, key, 3600000))
                return false;
        return !record.contains("ttfb") || measurement(record, "ttfb", 3600000);
    }
    if (record["kind"] != "interface" || !isString(record, "name", 15) ||
        !validInterface(record["name"].toString()) || !integer(record, "carrier", -1, 1) ||
        !isString(record, "ap", 64))
        return false;
    for (const auto key : {"rx", "tx", "rxDrops", "txDrops", "rxErrors", "txErrors"})
        if (!measurement(record, key, 1e20))
            return false;
    return true;
}
} // namespace

void History::createDiagnosticTables() {
    execute(db_, R"SQL(
CREATE TABLE IF NOT EXISTS diagnostic_settings(
 session INTEGER PRIMARY KEY REFERENCES sessions(id),url TEXT NOT NULL,interface TEXT NOT NULL,seconds INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS diagnostic_records(
 session INTEGER NOT NULL REFERENCES sessions(id),kind TEXT NOT NULL,t INTEGER NOT NULL,data TEXT NOT NULL,
 score REAL NOT NULL, flag INTEGER NOT NULL, PRIMARY KEY(session,kind,t)) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS diagnostic_bins(
 session INTEGER NOT NULL,kind TEXT NOT NULL,span INTEGER NOT NULL,b INTEGER NOT NULL,
 peak REAL NOT NULL,peak_t INTEGER NOT NULL,event_t INTEGER NOT NULL,
 PRIMARY KEY(session,kind,span,b)) WITHOUT ROWID;
)SQL");
}

QVariantMap History::diagnosticsSettings() {
    QVariantMap result;
    if (!open())
        return result;
    error_.clear();
    try {
        Statement q(db_, "SELECT session,url,interface,seconds FROM diagnostic_settings");
        while (q.next())
            result.insert(
                QString::number(q.integer(0)),
                QVariantMap{{"url", q.string(1)}, {"interface", q.string(2)}, {"seconds", q.integer(3)}});
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return result;
}

bool History::saveDiagnostics(qint64 id, const QString &url, const QString &interface, int seconds) {
    if (!open())
        return false;
    error_.clear();
    try {
        if (seconds < 5 || seconds > 3600 || (!url.isEmpty() && !validUrl(url)) ||
            (!interface.isEmpty() && !validInterface(interface)))
            throw std::runtime_error("Choose a valid HTTP(S) URL without user information or fragment, an "
                                     "interface, and a 5–3600 second service interval.");
        if (url.isEmpty() && interface.isEmpty()) {
            Statement q(db_, "DELETE FROM diagnostic_settings WHERE session=?");
            q.integer(1, id);
            q.next();
        } else {
            Statement q(db_, "INSERT OR REPLACE INTO diagnostic_settings VALUES(?,?,?,?)");
            q.integer(1, id);
            q.string(2, url);
            q.string(3, interface);
            q.integer(4, seconds);
            q.next();
        }
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::appendDiagnostic(qint64 id, const QVariantMap &record) {
    if (!open())
        return false;
    error_.clear();
    bool saving = false;
    try {
        const auto data = QJsonDocument(QJsonObject::fromVariantMap(record)).toJson(QJsonDocument::Compact);
        if (data.size() > 8192 || !validRecord(record))
            throw std::runtime_error("Invalid diagnostic observation.");
        const QString kind = record["kind"].toString();
        const qint64 t = record["t"].toLongLong();
        const double score = kind == "service" ? record["total"].toDouble()
                                               : std::max(record["rx"].toDouble(), record["tx"].toDouble());
        const bool event = !record["event"].toString().isEmpty();
        execute(db_, "SAVEPOINT diagnostic_sample");
        saving = true;
        Statement q(
            db_, "INSERT INTO diagnostic_records VALUES(?,?,?,?,?,?) ON CONFLICT(session,kind,t) DO NOTHING");
        q.integer(1, id);
        q.string(2, kind);
        q.integer(3, t);
        q.string(4, QString::fromUtf8(data));
        q.number(5, score);
        q.integer(6, event);
        q.next();
        if (sqlite3_changes(db_)) {
            Statement bin(db_, "INSERT INTO diagnostic_bins VALUES(?,?,?,?,?,?,?) "
                               "ON CONFLICT(session,kind,span,b) DO UPDATE SET "
                               "peak_t=CASE WHEN excluded.peak>=peak THEN excluded.peak_t ELSE peak_t END,"
                               "peak=max(peak,excluded.peak),event_t=max(event_t,excluded.event_t)");
            for (auto span : binSpans) {
                bin.reset();
                bin.integer(1, id);
                bin.string(2, kind);
                bin.integer(3, span);
                bin.integer(4, t / span * span);
                bin.number(5, score);
                bin.integer(6, t);
                bin.integer(7, event ? t : -1);
                bin.next();
            }
        } else {
            Statement existing(db_, "SELECT data FROM diagnostic_records WHERE session=? AND kind=? AND t=?");
            existing.integer(1, id);
            existing.string(2, kind);
            existing.integer(3, t);
            if (!existing.next() || existing.string(0).toUtf8() != data)
                throw std::runtime_error("Conflicting diagnostic observations share a timestamp.");
        }
        execute(db_, "RELEASE diagnostic_sample");
        return true;
    } catch (const std::exception &e) {
        if (saving)
            sqlite3_exec(db_, "ROLLBACK TO diagnostic_sample; RELEASE diagnostic_sample", nullptr, nullptr,
                         nullptr);
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::eachDiagnostic(qint64 id, qint64 from, qint64 through,
                             const std::function<void(const QVariantMap &)> &visit) {
    if (!open())
        return false;
    error_.clear();
    try {
        for (const auto kind : {"service", "interface"}) {
            Statement q(db_, "SELECT data FROM diagnostic_records WHERE session=? AND kind=? AND t BETWEEN ? "
                             "AND ? ORDER BY t");
            q.integer(1, id);
            q.string(2, kind);
            q.integer(3, from);
            q.integer(4, through);
            while (q.next())
                visit(decode(q.string(0)));
        }
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantList History::diagnosticTracks(qint64 id, qint64 from, qint64 through, int pixels) {
    QVariantList tracks;
    if (!open() || through < from)
        return tracks;
    error_.clear();
    pixels = std::clamp(pixels, 1, 1500);
    const auto duration = std::max(1LL, through - from + 1);
    qint64 span = 0;
    for (const auto step : binSpans)
        if (step <= duration / pixels)
            span = step;
    try {
        Transaction tx(db_, false);
        for (const auto kind : {"service", "interface"}) {
            struct Point {
                double peak = -2;
                QVariantMap record, event;
            };
            std::map<int, Point> columns;
            auto add = [&](QString json, double score, bool actualEvent) {
                if (json.isEmpty())
                    return;
                auto record = decode(json);
                const int x = std::clamp(int(double(record["t"].toLongLong() - from) / duration * pixels), 0,
                                         pixels - 1);
                auto &bucket = columns[x];
                if (actualEvent)
                    bucket.event = record;
                if (score >= bucket.peak) {
                    bucket.peak = score;
                    bucket.record = std::move(record);
                }
            };
            auto raw = [&](qint64 a, qint64 b) {
                if (a > b)
                    return;
                Statement q(db_, "SELECT data,score,flag FROM diagnostic_records WHERE session=? AND kind=? "
                                 "AND t BETWEEN ? AND ? ORDER BY t");
                q.integer(1, id);
                q.string(2, kind);
                q.integer(3, a);
                q.integer(4, b);
                while (q.next())
                    add(q.string(0), q.number(1), q.integer(2));
            };
            const qint64 firstBin = span ? (from + span - 1) / span * span : 0;
            const qint64 pastBin = span ? (through + 1) / span * span : 0;
            if (!span || pastBin <= firstBin)
                raw(from, through);
            else {
                raw(from, firstBin - 1);
                Statement q(
                    db_,
                    "SELECT p.data,b.peak,e.data,e.score FROM diagnostic_bins b "
                    "JOIN diagnostic_records p ON p.session=b.session AND p.kind=b.kind AND p.t=b.peak_t "
                    "LEFT JOIN diagnostic_records e ON e.session=b.session AND e.kind=b.kind AND "
                    "e.t=b.event_t "
                    "WHERE b.session=? AND b.kind=? AND b.span=? AND b.b>=? AND b.b<? ORDER BY b.b");
                q.integer(1, id);
                q.string(2, kind);
                q.integer(3, span);
                q.integer(4, firstBin);
                q.integer(5, pastBin);
                while (q.next()) {
                    add(q.string(0), q.number(1), false);
                    add(q.string(2), q.number(3), true);
                }
                raw(pastBin, through);
            }
            if (columns.empty())
                continue;
            QVariantList points;
            for (auto &[x, p] : columns) {
                if (!p.event.isEmpty())
                    p.record["marker"] = p.event;
                points.append(std::move(p.record));
            }
            tracks << QVariantMap{{"kind", QString(kind)}, {"points", points}};
        }
        tx.commit();
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return tracks;
}
