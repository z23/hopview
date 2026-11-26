#include "history.h"
#include "historycache.h"
#include "routeevents.h"
#include "sqlhelpers.h"
#include <QJsonObject>
#include <QJsonDocument>

using namespace hopview_sql;

namespace {
QString json(const QVariantMap &map) {
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QVariantMap decode(const QString &value) {
    return QJsonDocument::fromJson(value.toUtf8()).object().toVariantMap();
}
} // namespace

// The caller holds a write transaction. Rebuild lazily after out-of-order arrivals.
bool History::updateRouteIndex(qint64 id, qint64 firstNew) {
    qint64 cursor = -1, until = 0;
    constexpr qint64 hour = 3600000;
    constexpr qint64 checkpointSpacingMs = 60000;
    auto &c = cache();
    RouteEvents detector;
    qint64 lastCheckpoint = -1;
    QString previousStateJson;
    Statement cutoff(db_, "SELECT raw_before FROM retention");
    cutoff.next();
    const auto rawBefore = cutoff.integer(0);
    auto restoreBefore = [&](qint64 t) {
        Statement checkpoint(db_, "SELECT t,state FROM route_checkpoints WHERE session=?1 AND hour<=?2 "
                                  "AND t<?3 AND t>?4 ORDER BY hour DESC LIMIT 1");
        checkpoint.integer(1, id);
        checkpoint.integer(2, t / hour);
        checkpoint.integer(3, t);
        checkpoint.integer(4, cursor);
        if (checkpoint.next()) {
            cursor = checkpoint.integer(0);
            previousStateJson = checkpoint.string(1);
            detector.restore(decode(previousStateJson));
            lastCheckpoint = cursor;
        }
    };
    Statement get(db_, "SELECT cursor,state FROM route_index WHERE session=?");
    get.integer(1, id);
    if (get.next()) {
        cursor = get.integer(0);
        if (firstNew >= 0 && firstNew <= cursor) {
            c.routeStates.remove(id);
            cursor = -1;
            previousStateJson.clear();
            restoreBefore(firstNew);
            // A late packet may change transitions after the checkpoint. Preserve
            // intervals whose archived raw samples can no longer reproduce them.
            for (auto table : {"route_transitions", "route_gaps", "route_checkpoints"}) {
                const auto time =
                    QByteArray(table) == "route_transitions" ? "json_extract(data,'$.confirmedMs')" : "t";
                const auto sql = QByteArray("DELETE FROM ") + table + " AS e WHERE session=?1 AND " + time +
                                 ">?2 AND (t>=?3 OR EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=?1 "
                                 "AND e.t BETWEEN p.start_ms AND p.end_ms))";
                Statement clear(db_, sql.constData());
                clear.integer(1, id);
                clear.integer(2, cursor);
                clear.integer(3, rawBefore);
                clear.next();
            }
        } else {
            previousStateJson = get.string(1);
            auto cachedIt = c.routeStates.find(id);
            // Another connection can rewind for a late sample and replay to this same cursor
            // with a different detector state, so the cursor alone does not identify it.
            if (cachedIt != c.routeStates.end() && cachedIt->cursor == cursor &&
                cachedIt->state == previousStateJson) {
                detector = cachedIt->detector;
                lastCheckpoint = cachedIt->lastCheckpoint;
            } else {
                detector.restore(decode(previousStateJson));
            }
        }
    }
    get.reset();
    Statement end(db_, "SELECT max(t) FROM samples WHERE session=? AND hop=0");
    end.integer(1, id);
    end.next();
    until = end.integer(0);
    if (cursor >= until)
        return false;
    Statement next(db_, "SELECT t FROM samples WHERE session=? AND hop=0 AND t>? ORDER BY t LIMIT 1");
    next.integer(1, id);
    next.integer(2, cursor);
    if (!next.next())
        return false;
    const auto first = next.integer(0);
    // Before crossing into a new UTC hour, checkpoint the completed hour's final state. Write it
    // even when this connection believes it has: another connection's rebuild may have removed it.
    if (cursor >= 0 && first / hour != cursor / hour) {
        Statement flushCkpt(db_, "INSERT OR REPLACE INTO route_checkpoints VALUES(?,?,?,?)");
        flushCkpt.integer(1, id);
        flushCkpt.integer(2, cursor / hour);
        flushCkpt.integer(3, cursor);
        flushCkpt.string(4, previousStateJson.isEmpty() ? json(detector.state()) : previousStateJson);
        flushCkpt.next();
        lastCheckpoint = cursor;
    }
    if (rawBefore && cursor < rawBefore && first / hour != cursor / hour)
        restoreBefore(first);
    // Finish one UTC hour before entering the next so a frozen segment can
    // supply the exact detector state even after its samples have expired.
    const qint64 finalTime = until;
    until = std::min(until, (first / hour + 1) * hour - 1);
    auto before = cursor;
    QVariantList events;
    bool more = false;
    if (!routePage(id, detector, cursor, until, events, more))
        throw std::runtime_error(error_.toStdString());
    Statement insert(db_, "INSERT OR REPLACE INTO route_transitions VALUES(?,?,?)");
    for (const auto &value : events) {
        const auto e = value.toMap();
        insert.reset();
        insert.integer(1, id);
        insert.integer(2, e["firstMs"].toLongLong());
        insert.string(3, json(e));
        insert.next();
    }
    Statement gap(db_, "INSERT OR REPLACE INTO route_gaps "
                       "SELECT ?1,t,prev FROM (SELECT t,lag(t) OVER (ORDER BY t) AS prev FROM samples "
                       "WHERE session=?1 AND hop=0 AND t BETWEEN ?2 AND ?3) "
                       "WHERE t-prev > max(3000,3*(SELECT interval_ms FROM sessions WHERE id=?1))");
    gap.integer(1, id);
    gap.integer(2, before);
    gap.integer(3, cursor);
    gap.next();
    Statement put(db_, "INSERT OR REPLACE INTO route_index VALUES(?,?,?)");
    put.integer(1, id);
    put.integer(2, cursor);
    const auto state = json(detector.state());
    put.string(3, state);
    put.next();
    const bool needCheckpoint = before < 0 || cursor / hour != before / hour ||
                                cursor >= (first / hour + 1) * hour - 1 ||
                                lastCheckpoint < 0 || cursor - lastCheckpoint >= checkpointSpacingMs;
    if (needCheckpoint) {
        Statement checkpoint(db_, "INSERT OR REPLACE INTO route_checkpoints VALUES(?,?,?,?)");
        checkpoint.integer(1, id);
        checkpoint.integer(2, cursor / hour);
        checkpoint.integer(3, cursor);
        checkpoint.string(4, state);
        checkpoint.next();
        lastCheckpoint = cursor;
    }
    if (c.routeStates.size() > 256 && !c.routeStates.contains(id))
        c.routeStates.clear();
    c.routeStates[id] = {cursor, lastCheckpoint, state, std::move(detector)};
    return more || cursor < finalTime;
}

bool History::advanceRouteIndex(qint64 id, bool &more) {
    more = false;
    if (!open())
        return false;
    error_.clear();
    try {
        Statement needed(db_, "SELECT EXISTS(SELECT 1 FROM samples WHERE session=?1 AND hop=0 "
                              "AND t>coalesce((SELECT cursor FROM route_index WHERE session=?1),-1))");
        needed.integer(1, id);
        if (!needed.next() || !needed.integer(0))
            return true;
        needed.reset();
        Transaction tx(db_, true);
        more = updateRouteIndex(id, -1);
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::indexedRoutes(qint64 id, qint64 &cursor, QVariantList &events, bool &more) {
    events.clear();
    more = false;
    if (!open())
        return false;
    error_.clear();
    try {
        if (!advanceRouteIndex(id, more))
            return false;
        Transaction tx(db_, false);
        Statement list(db_,
                       "SELECT t,data FROM route_transitions WHERE session=? AND t>? ORDER BY t LIMIT 256");
        list.integer(1, id);
        list.integer(2, cursor);
        while (list.next()) {
            cursor = list.integer(0);
            events << decode(list.string(1));
        }
        more |= events.size() == 256;
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantList History::timelineEvents(qint64 id, qint64 from, qint64 through) {
    QVariantList events;
    if (!open())
        return events;
    error_.clear();
    try {
        Statement q(
            db_,
            "SELECT * FROM ("
            "SELECT t,'route' AS kind,'Hops '||json_extract(data,'$.changedHops')||CASE "
            "WHEN json_extract(data,'$.knownAlternate') THEN ' returned to a known responder' "
            "ELSE ' changed' END AS label FROM route_transitions WHERE session=?1 AND t BETWEEN ?2 AND ?3 "
            "UNION ALL SELECT end_ms,'alert',CASE WHEN reason=4 THEN 'Recovered' ELSE 'Destination threshold "
            "crossed' END "
            "FROM alert_events WHERE session=?1 AND end_ms BETWEEN ?2 AND ?3 "
            "UNION ALL SELECT t,'note',body FROM notes WHERE session=?1 AND t BETWEEN ?2 AND ?3 "
            "UNION ALL SELECT t,kind,coalesce(nullif(json_extract(data,'$.event'),''),"
            "nullif(json_extract(data,'$.error'),''),'HTTP '||json_extract(data,'$.status')) FROM "
            "diagnostic_records "
            "WHERE session=?1 AND t BETWEEN ?2 AND ?3 AND (flag OR (kind='service' AND "
            "(json_extract(data,'$.error')<>'' OR json_extract(data,'$.status')>=400))) "
            "UNION ALL SELECT t,'gap','Collection gap of '||round((t-previous_ms)/1000.0,1)||' s' "
            "FROM route_gaps WHERE session=?1 AND t BETWEEN ?2 AND ?3) ORDER BY t DESC LIMIT 501");
        q.integer(1, id);
        q.integer(2, from);
        q.integer(3, through);
        while (q.next())
            events << QVariantMap{{"t", q.integer(0)}, {"kind", q.string(1)}, {"text", q.string(2)}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return events;
}

QVariantList History::routeTransitions(qint64 id, qint64 from, qint64 through, int limit) {
    QVariantList events;
    if (!open())
        return events;
    error_.clear();
    try {
        const char *sql = limit > 0
                              ? "SELECT t, 'Hops '||json_extract(data,'$.changedHops')||CASE "
                                "WHEN json_extract(data,'$.knownAlternate') THEN ' returned to a known responder' "
                                "ELSE ' changed' END FROM route_transitions WHERE session=?1 AND t BETWEEN ?2 AND ?3 "
                                "ORDER BY t DESC LIMIT ?4"
                              : "SELECT t, 'Hops '||json_extract(data,'$.changedHops')||CASE "
                                "WHEN json_extract(data,'$.knownAlternate') THEN ' returned to a known responder' "
                                "ELSE ' changed' END FROM route_transitions WHERE session=?1 AND t BETWEEN ?2 AND ?3 "
                                "ORDER BY t DESC";
        Statement q(db_, sql);
        q.integer(1, id);
        q.integer(2, from);
        q.integer(3, through);
        if (limit > 0)
            q.integer(4, limit);
        while (q.next())
            events << QVariantMap{{"t", q.integer(0)}, {"kind", QStringLiteral("route")}, {"text", q.string(1)}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return events;
}
