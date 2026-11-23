#include "history.h"
#include "collectormanagement.h"
#include "sqlhelpers.h"
#include <QJsonArray>
#include <QUuid>
#include "incident.h"
#include <QFile>
#include "routeevents.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>

namespace {
void require(bool pass, const char *msg) {
    if (!pass)
        throw std::runtime_error(msg);
}

struct Expected {
    qint64 sent = 0, received = 0;
    double low = -1, high = -1, last = -1, lastOk = -1, sum = 0, transitions = 0;
    QString address;
    void add(const Sample &s) {
        ++sent;
        if (!s.address.isEmpty())
            address = s.address;
        last = s.rttMs;
        if (last < 0)
            return;
        if (received++)
            transitions += std::abs(last - lastOk);
        lastOk = last;
        low = low < 0 ? last : std::min(low, last);
        high = std::max(high, last);
        sum += last;
    }
    void compare(const QVariantMap &row, bool full) const {
        auto check = [&](const char *field, double value) {
            require(row.contains(field), "Missing field");
            if (std::abs(row.value(field).toDouble() - value) > 1e-8)
                throw std::runtime_error(QString("Incorrect %1, got %2 expected %3")
                                             .arg(field)
                                             .arg(row.value(field).toDouble())
                                             .arg(value)
                                             .toStdString());
        };
        check("sent", sent);
        check("min", low);
        check("max", high);
        check("avg", received ? sum / received : -1);
        check("loss", 100.0 * (sent - received) / sent);
        if (full) {
            check("cur", last);
            check("jitter", received > 1 ? transitions / (received - 1) : -1);
            require(row["address"].toString() == address, "Historical responder");
        }
    }
};

void test(const QString &file) {
    constexpr qint64 base = 1720012345000;
    History writer(file);
    const qint64 id = writer.createSession("example.test", 4, 1000);
    require(id > 0, "Creating session");
    History reader(file);
    require(reader.open(), "Second connection");
    constexpr int hops = 3, rounds = 7300;
    QVector<Sample> fixture, late, normal;
    for (int i = 0; i < rounds; ++i) {
        for (int hop = 1; hop <= hops; ++hop) {
            const auto time = base + 1000LL * i;
            const bool loss = (i % 31 == 0 || (i >= 160 && i <= 180 && hop == 2));
            const QString address = loss ? QString() : QString("192.0.2.%1").arg(hop + (i > 2400 ? 10 : 0));
            Sample s{id, time, hop, address, loss ? -1.0 : double((i * hop) % 227), !loss && hop == 3};
            fixture.append(s);
            (i == 135 || i == 3602 ? late : normal).append(s);
        }
    }
    std::reverse(normal.begin(), normal.end());
    require(writer.append(normal) && writer.append(late) && writer.append(late),
            "Batched, late and repeated probes");
    auto failed = fixture[0];
    failed.timeMs += 777;
    auto invalid = failed;
    invalid.session = id + 10;
    require(!writer.append({failed, invalid}), "Failed batch rolls back");
    auto atomic = reader.view(id, base, base + 900, base, base + 900, {1}, 3);
    require(atomic["rows"].toList().first().toMap()["sent"].toInt() == 1, "No partial append");
    const qint64 start = base + 3751, end = base + 7285765;
    const qint64 fStart = base + 109075, fEnd = base + 6485552;
    const int width = 83;
    const auto view = reader.view(id, start, end, fStart, fEnd, {1, 0, 3}, width);
    require(reader.error().isEmpty(), "Reading historical view");
    const auto rows = view["rows"].toList(), charts = view["charts"].toList();
    require(rows.size() == 3 && charts.size() == 2, "Requested charts include destination once");
    Expected focus[3];
    QVector<QVector<Expected>> pixels(3, QVector<Expected>(width));
    for (const auto &s : fixture) {
        if (s.timeMs >= fStart && s.timeMs <= fEnd)
            focus[s.hop - 1].add(s);
        if (s.timeMs >= start && s.timeMs <= end)
            pixels[s.hop - 1][(s.timeMs - start) * width / (end - start + 1)].add(s);
    }
    for (int i = 0; i < 3; ++i)
        focus[i].compare(rows[i].toMap(), true);
    for (const auto &chart : charts) {
        const auto c = chart.toMap();
        const int hop = c["hop"].toInt();
        require(hop == 1 || hop == 3, "Destination and selected hop");
        const auto buckets = c["buckets"].toList();
        require(buckets.size() == width, "One bucket per occupied pixel");
        for (const auto &b : buckets) {
            const auto map = b.toMap();
            pixels[hop - 1][map["x"].toInt()].compare(map, false);
        }
    }
    // The historical selection contains a different responder than the live end.
    auto before = reader.view(id, base, base + 200000, base + 100500, base + 101500, {}, 4)["rows"].toList();
    require(before[0].toMap()["address"].toString() == "192.0.2.1", "Early responder");
    auto loss =
        reader.view(id, base, base + 200000, base + 165000, base + 171999, {2}, 5)["rows"].toList()[1].toMap();
    require(loss["loss"].toDouble() == 100 && loss["avg"].toDouble() == -1, "All-loss focus");
    require(writer.setInterval(id, 375), "Edit probe interval");
    auto session = reader.sessions().first().toMap();
    require(session["startMs"].toLongLong() == fixture.first().timeMs &&
                session["endMs"].toLongLong() == fixture.last().timeMs && session["intervalMs"].toInt() == 375,
            "Persistent session metadata");

    require(writer.addNote(id, base, "old") && writer.addNote(id, base, "updated") &&
                writer.addNote(id, base + 5000, "outside view"),
            "Save annotations");
    auto notes = reader.view(id, base, base + 50, base, base, {}, 1)["notes"].toList();
    require(notes.size() == 1 && notes.first().toMap()["text"] == "updated", "Notes follow viewport");
    require(writer.addNote(id, base, QString()), "Delete annotation");
    require(reader.view(id, base, base + 50, base, base, {}, 1)["notes"].toList().isEmpty(), "Deleted note");
}

void testRouteChange(const QString &file) {
    History h(file);
    qint64 id = h.createSession("changing.example", 4, 1000);
    constexpr qint64 first = 3600000;
    // The target moves from TTL 2 to TTL 3, then disappears. Intermediate
    // RTTs must never enter the endpoint timeline, even at the old target TTL.
    QVector<Sample> probes{{id, first, 1, "router-a", 1, false},
                           {id, first, 2, "target", 10, true},
                           {id, first + 10000, 1, "router-b", 2, false},
                           {id, first + 10000, 2, "forwarder", 900, false},
                           {id, first + 10000, 3, "target", 20, true},
                           {id, first + 20000, 1, "router-a", 1, false},
                           {id, first + 20000, 2, "forwarder", 950, false},
                           {id, first + 20000, 3, {}, -1, false},
                           {id, first + 30000, 1, "router-a", 1, false},
                           {id, first + 30000, 2, "forwarder", 900, false}};
    require(h.append(probes), "Write varying route");
    // A late portion of a split batch upgrades just this round to success.
    require(h.append({{id, first + 30000, 3, "target", 15, true}}), "Reassemble split destination");
    const auto v = h.view(id, first, first + 59999, first, first + 59999, {0, 2}, 1);
    const auto c = v["charts"].toList().first().toMap();
    require(v["timeMax"].toDouble() == 950, "Maximum includes every chart");
    require(c["hop"].toInt() == 3 && c["destination"].toBool(), "Endpoint tracks TTL");
    auto b = c["buckets"].toList().first().toMap();
    require(b["sent"].toInt() == 4 && b["loss"].toDouble() == 25 && b["min"].toDouble() == 10 &&
                b["max"].toDouble() == 20 && b["avg"].toDouble() == 15,
            "Only target outcomes contribute across reroutes");
    auto row = v["rows"].toList().first().toMap();
    require(row["routeChanged"].toBool() && row["firstAddress"] == "router-a" && row["address"] == "router-a",
            "A route change and reversal survive aggregation");
    auto old = h.view(id, first, first + 9000, first, first + 9000, {0}, 1);
    require(old["charts"].toList()[0].toMap()["hop"].toInt() == 2, "Earlier destination TTL");
    require(!old["rows"].toList()[0].toMap()["routeChanged"].toBool(), "No change before reroute");
}

void testRouteEvents(const QString &file) {
    History writer(file);
    const auto id = writer.createSession("2001:db8::1", 6, 1000);
    const QString target = "2001:db8::1";
    QVector<Sample> samples;
    for (int i = 0; i < 540; ++i) {
        const qint64 stamp = 10000LL + i * 1000;
        // One isolated reply, alternation, and a failed two-round candidate.
        const bool timeout = i == 496 || i == 509;
        QString first = (i == 487 || (i >= 490 && i <= 499 && i % 2) || i >= 508) ? "new" : "old";
        samples.append({id, stamp, 1, timeout ? QString() : first, timeout ? -1. : 3., false});
        const bool shortPath = i >= 520 && i < 529;
        // The former destination disappears during the extension at round 529.
        const bool silent = i >= 529 && i < 533;
        samples.append({id, stamp, 2, shortPath ? target : silent ? QString() : "middle",
                        silent ? -1. : shortPath ? 18. : 7., shortPath});
        if (!shortPath) {
            const bool dropped = (i >= 503 && i <= 505) || i == 508 || i == 510;
            samples.append({id, stamp, 3, dropped ? QString() : target,
                            dropped ? -1. : i >= 510 ? 34. : 16., !dropped});
        }
    }
    std::reverse(samples.begin(), samples.end());
    require(writer.append(samples), "Seed route events");
    // Reopen as an existing recording and process across a page boundary.
    History read(file);
    RouteEvents detector;
    qint64 cursor = -1, until = 0;
    bool more = false;
    QVariantList page, all;
    require(read.routePage(id, detector, cursor, until, page, more) && more && page.isEmpty(),
            "Alternation, a one-round switch, and timeouts are not sustained reroutes");
    const qint64 frozen = until;
    require(writer.append({{id, until+1000, 1, "later", 2., false}}), "Concurrent append");
    require(read.routePage(id, detector, cursor, until, page, more) && !more && until == frozen,
            "Finish snapshot without following later arrivals");
    all.append(page);
    require(all.size() == 3, "One sustained responder change plus shorter and longer paths");
    const auto e = all[0].toMap();
    require(e["firstMs"].toLongLong() == 520000 && e["confirmedMs"].toLongLong() == 522000 &&
            e["startMs"].toLongLong() == 517000 && e["changedHops"] == "1", "Sub-page timing and drill window");
    require(e["beforeRtt"].toDouble() == 16. && e["afterRtt"].toDouble() == 34. &&
            e["beforeN"].toInt() == 2 && e["afterN"].toInt() == 2, "RTT uses available destination replies");
    auto first = e["path"].toList().first().toMap();
    require(first["before"] == "old" && first["after"] == "new" && first["changed"].toBool(),
            "Stable topology before and after the switch");
    auto shorter = all[1].toMap()["path"].toList(), longer = all[2].toMap()["path"].toList();
    require(shorter[1].toMap()["isTarget"].toBool() && !shorter[2].toMap()["inAfter"].toBool(),
            "Destination confirmation shortens path");
    require(longer[1].toMap()["wasTarget"].toBool() && longer[1].toMap()["after"].toString().isEmpty() &&
            longer[2].toMap()["isTarget"].toBool() && !longer[2].toMap()["inBefore"].toBool(),
            "Silent intermediate TTL in extended path stays unknown");

    RouteEvents interrupted;
    for (int i = 0; i < 220; ++i) {
        // Interleaved A/B and A/A/B/B responses cannot establish a new stable path.
        const QString address = i < 4 ? "a" : i < 100 ? (i % 2 ? "b" : "a")
                              : i < 218 ? (i % 4 < 2 ? "a" : "b") : "c";
        require(interrupted.observe({i * 1000LL, {{1,address}, {2,target}}, 2, 5.}).isEmpty(),
                "Alternating responders do not generate route events");
    }
    const qint64 resumed = 24 * 3600000LL;
    for (int i = 0; i < 3; ++i) {
        auto event = interrupted.observe({resumed + i*1000, {{1,"c"}, {2,target}}, 2, 10.});
        if (i < 2)
            require(event.isEmpty(), "A pause interrupts the in-progress candidate");
        else
            require(event["firstMs"].toLongLong() == resumed && event["startMs"].toLongLong() == resumed &&
                    event["beforeN"].toInt() == 0 && event["afterN"].toInt() == 3,
                    "Post-resume change has new evidence and no stale RTT comparison");
    }
}

void testDestinationAlerts(const QString &file) {
    History writer(file);
    const auto id = writer.createSession("target.test", 4, 1000);
    require(id > 0 && writer.setAlertRule(id, 100, 50, 2), "Enable destination alerts");
    qint64 t = 10000;
    QList<qint64> emitted, ids;
    QVector<Sample> previous;
    auto window = [&](double rtt) {
        QVector<Sample> batch;
        for (int i = 0; i < 3; ++i, t += 1000) {
            batch.append({id, t, 1, {}, -1, false});  // Upstream loss alone is irrelevant.
            batch.append({id, t, 2, rtt < 0 ? QString() : "target", rtt, rtt >= 0});
        }
        require(writer.append(batch, &emitted), "Record destination alert window");
        ids.append(emitted);
        previous = batch;
    };
    window(500); window(2); window(500);
    require(ids.isEmpty(), "Isolated spikes and intermediate loss are quiet");
    t += 100000;
    window(400);
    require(ids.isEmpty(), "A collection gap breaks the candidate");
    const qint64 onset = t - 3000;
    window(500); window(500);
    require(ids.size() == 1, "Two bad windows open exactly one incident");
    require(writer.append(previous, &emitted) && emitted.isEmpty(), "Replay does not reissue alerts");
    History reopened(file);
    require(reopened.open(), "Reopen monitoring history");
    auto events = reopened.alertEvents(-1);
    require(events.size() == 1 && events[0].toMap()["label"] == "High latency" &&
            events[0].toMap()["startMs"].toLongLong() == onset, "Persisted latency event and drill range");
    window(2); window(500); window(2);
    require(ids.size() == 1, "One healthy window does not recover a flapping incident");
    window(2);
    require(ids.size() == 2 && reopened.alertEvents(-1)[0].toMap()["recovered"].toBool(), "Sustained recovery");
    window(-1); window(-1);
    require(ids.size() == 3 && reopened.alertEvents(-1)[0].toMap()["label"] == "Packet loss",
            "Timeout-only rounds raise destination loss");
    window(2); window(2);
    require(ids.size() == 4, "Loss incident recovers");
    require(writer.setAlertRule(id, 0, 0, 2), "Disable alerts");
    window(-1); window(-1);
    require(ids.size() == 4 && reopened.alertEvents(-1).size() == 4, "Disabling keeps evidence and silences new alerts");
    require(writer.saveBackground(true, {id}), "Enable unattended resume");
    const auto settings = reopened.monitoringSettings();
    require(settings["background"].toBool() && settings["active"].toList().contains(id), "Durable recording intent");
    require(writer.setTabClosed(id, true) && reopened.monitoringSettings()["active"].toList().isEmpty(),
            "Archived traces stay stopped on restart");
}

void testUpgrade(const QString &file) {
    qint64 id;
    {
        History h(file);
        id = h.createSession("old.example", 4, 1000);
        require(h.append({{id, 10000, 1, "a", 1, false},
                          {id, 10000, 2, "target", 3, true},
                          {id, 11000, 1, "b", 2, false},
                          {id, 11000, 2, {}, -1, false}}),
                "Seed existing recording");
    }
    sqlite3 *db = nullptr;
    require(sqlite3_open(file.toUtf8().constData(), &db) == SQLITE_OK, "Open upgrade fixture");
    const int rc = sqlite3_exec(db,
                                "ALTER TABLE bins DROP COLUMN first_address;"
                                "ALTER TABLE bins DROP COLUMN route_changed; DELETE FROM samples WHERE hop=0",
                                nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(rc == SQLITE_OK, "Prepare older schema");
    History h(file);
    const auto view = h.view(id, 0, 59999, 0, 59999, {0}, 1);
    const auto rows = view["rows"].toList(), charts = view["charts"].toList();
    require(rows.size() == 2 && charts.size() == 1, "Recover existing session");
    require(rows[0].toMap()["routeChanged"].toBool(), "Recover route transitions");
    const auto b = charts[0].toMap()["buckets"].toList().first().toMap();
    require(b["sent"].toInt() == 2 && b["avg"].toDouble() == 3 && b["loss"].toDouble() == 50,
            "Reconstruct destination and lost rounds");
}

void testComparisonEvidence(const QTemporaryDir &dir) {
    const auto db = dir.filePath("group-source.db"), file = dir.filePath("two-locations.hopview");
    const auto csv = dir.filePath("group.csv");
    const qint64 t = 1700000000000;
    History h(db);
    auto primary = h.createSession("edge.example", 6, 1000, "tcp", 443);
    auto peer = h.createSession("edge.example", 4, 1000, "udp", 5001);
    require(h.setSessionOrigin(peer, "Melbourne", "192.0.2.1", -75, 9, t + 2000), "Set collector metadata");
    require(h.append({{primary, t, 1, "::1", 10, true},
                      {primary, t + 1000, 1, "::1", 12, true},
                      {peer, t, 1, "192.0.2.1", 55, true},
                      {peer, t + 1000, 1, {}, -1, false}}),
            "Write site probes");
    require(h.addNote(primary, t + 20, "primary annotation") && h.addNote(peer, t + 120, "peer annotation"),
            "Write notes");
    const QVariantMap service{{"kind", "service"},
                              {"t", t + 400},
                              {"periodMs", 1000},
                              {"event", "HTTP 503"},
                              {"url", "https://edge.example/"},
                              {"address", "192.0.2.1"},
                              {"status", 503},
                              {"family", 4},
                              {"error", ""},
                              {"dns", 1.0},
                              {"connect", 2.0},
                              {"tls", 3.0},
                              {"firstByte", 7.0},
                              {"total", 13.5}};
    require(h.appendDiagnostic(peer, service), "Write service observation");
    require(h.saveGroup("Offices", {primary, peer}, {0, 1}) && h.setTabClosed(peer, true),
            "Save group before archive");
    History reopened(db);
    QList<qint64> members;
    QList<int> hops;
    require(reopened.groupNames() == QStringList{"Offices"} && reopened.openGroup("Offices", members, hops),
            "Persist group");
    require(members == QList<qint64>{primary, peer} && hops == QList<int>{0, 1} &&
                !reopened.sessions().first().toMap()["closed"].toBool(),
            "Restore original order and archived member");
    require(!h.saveGroup("Offices", {primary, 999999}, {0}) && h.openGroup("Offices", members, hops) &&
                members == QList<qint64>{primary, peer},
            "Rejected update preserves existing group");
    require(h.exportIncident(primary, t, t + 1000, file, false, {peer}, {1, 0}), "Export group replay");
    require(h.exportIncident(primary, t, t + 1000, csv, true, {peer}), "Export marked CSV");
    QFile readCsv(csv);
    require(readCsv.open(QIODevice::ReadOnly), "Read comparison CSV");
    const auto bytes = readCsv.readAll();
    require(bytes.count("\"probe\"") == 4 && bytes.count("\"note\",\"2023") == 2 &&
                bytes.contains("Melbourne") && bytes.contains("\"service\",\"2023") &&
                bytes.contains("HTTP 503"),
            "CSV includes all tracks, site and service evidence");

    History imported(dir.filePath("combined.db"));
    const auto existing = imported.createSession("existing", 4, 250);
    IncidentRange range;
    QString error;
    require(importIncident(file, imported, range, error, "London", -100), qPrintable(error));
    require(range.from == t - 100 && range.through == t + 900 && range.pinnedHops == QList<int>{1, 0} &&
                range.sessions.size() == 2 && !range.sessions.contains(existing),
            "Restore imported selection, alignment and pins");
    const auto sessionRows = imported.sessions();
    const auto primaryMeta = sessionRows[1].toMap(), peerMeta = sessionRows[0].toMap();
    require(primaryMeta["location"] == "London" && peerMeta["location"] == "Melbourne" &&
                peerMeta["resolvedAddress"] == "192.0.2.1" && peerMeta["clockOffsetMs"] == -75 &&
                peerMeta["clockErrorMs"] == 9 && peerMeta["manualCorrectionMs"] == -100 &&
                peerMeta["imported"].toBool() && !peerMeta["remoteLive"].toBool(),
            "Preserve site and distinguish observed versus applied offset");
    const auto view = imported.view(range.sessions[1], range.from, range.through, range.from, range.through,
                                    {0}, 200, true);
    require(view["notes"].toList().first().toMap()["timeMs"] == t + 20 &&
                view["charts"].toList().first().toMap()["notes"].toList().first().toMap()["text"] ==
                    "peer annotation",
            "Peer annotations retain ownership and corrected timestamps");
    QVariantList diagnostics;
    require(imported.eachDiagnostic(range.sessions[1], range.from, range.through,
                                    [&](const auto &v) { diagnostics.append(v); }) &&
                diagnostics.size() == 1 && diagnostics.first().toMap()["t"] == t + 300 &&
                diagnostics.first().toMap()["status"] == 503,
            "Merged historical service check remains aligned");

    QFile malformed(file);
    require(malformed.open(QIODevice::ReadWrite) && malformed.resize(malformed.size() - 12),
            "Truncate multi-trace export");
    malformed.close();
    require(!importIncident(file, imported, range, error) && imported.sessions() == sessionRows,
            "Malformed late record cannot leave partial traces in a live workspace");
    require(reopened.deleteGroup("Offices") && reopened.groupNames().isEmpty() &&
                reopened.sessions().size() == 2,
            "Removing a bookmark preserves recordings");
}

void testPeriodComparison(const QString &file) {
    History h(file);
    const auto id = h.createSession("dual.example",6,1000);
    const qint64 t = 1700000000000;
    QVector<Sample> batch;
    for (int i=1; i<=20; ++i) batch.append({id,t+i*1000,1,"2001:db8::a",double(i),true});
    for (int i=1; i<=6; ++i) {
        const auto stamp = t + (i <= 3 ? i+20 : i+100) * 1000;
        batch.append({id,stamp,1,i == 6 ? "2001:db8::b" : "",i == 6 ? 200. : -1.,i == 6});
    }
    require(h.append(batch),"Seed baseline percentiles and gaps");
    const auto rows = h.comparePeriods(id,t,t+20000,t+21000,t+106000,[]{return false;});
    require(rows.size() == 2,"Compare destination and real hop");
    const auto row = rows.last().toMap();
    const auto a = row["baseline"].toMap(), b = row["incident"].toMap();
    require(a["median"].toDouble()==10.5 && a["p95"].toDouble()==19 && a["sent"]==20,"Exact median and nearest-rank p95");
    require(b["received"]==1 && b["longest"]==3 && b["median"]==200 && b["p95"]==200 && row["changed"].toBool(),
            "Loss runs stop at a recording gap and responders change");
    const auto empty = h.comparePeriods(id,t-3,t-1,t,t+2000,[]{return false;}).first().toMap()["baseline"].toMap();
    require(empty["sent"]==0 && empty["p95"]==-1 && empty["loss"]==-1,"Distinguish missing period from loss");

    const auto slow = h.createSession("cadence.example", 4, 10000);
    const auto tail = t + 180000;
    require(h.append({{slow, t, 1, {}, -1, false},
                      {slow, t + 10000, 1, {}, -1, false},
                      {slow, t + 20000, 1, {}, -1, false},
                      {slow, tail, 1, {}, -1, false},
                      {slow, tail + 10000, 1, {}, -1, false}}) &&
                h.setInterval(slow, 250),
            "Record at ten seconds, then change active setting");
    auto longest = [&](qint64 session, qint64 first, qint64 last, int hop = 0) {
        const auto compared = h.comparePeriods(session, first, last, first, last, [] { return false; });
        for (const auto &value : compared) {
            const auto row = value.toMap();
            if (row["hop"].toInt() == hop)
                return row["incident"].toMap()["longest"].toInt();
        }
        return -1;
    };
    require(longest(slow, t, tail + 10000) == 3 && longest(slow, t + 10000, t + 20000) == 2,
            "Old cadence survives a new setting and a selection containing only two rounds");
    const auto sparse = h.createSession("sparse.example", 4, 10000);
    require(h.append({{sparse, t, 1, {}, -1, false}, {sparse, t + 10000, 1, {}, -1, false}}),
            "Write sparse history");
    require(longest(sparse, t, t + 10000) == 2 && h.setInterval(sparse, 1000) &&
                longest(sparse, t, t + 10000) == 1,
            "Use configured cadence when the recording has only two samples");
    const auto missing = h.createSession("shorter-route.example", 4, 60000);
    QVector<Sample> partial;
    for (int i = 0; i < 31; ++i) {
        partial.append({missing, t + i * 1000, 1, "192.0.2.1", 10, true});
        if (i % 10 == 0)
            partial.append({missing, t + i * 1000, 2, {}, -1, false});
    }
    require(h.append(partial) && longest(missing, t, t + 30000, 2) == 1,
            "Absent hops break streaks when faster rounds continue");
}

void testIncident(const QTemporaryDir &dir) {
    const auto db = dir.filePath("export.db");
    History source(db);
    const auto id = source.createSession("service.example", 6, 250, "tcp", 8443, "eth1");
    const qint64 t = 1700000000000;
    require(source.append({{id, t, 1, "::1", 1, false},
                           {id, t + 250, 1, "::1", 0.25, false},
                           {id, t + 250, 2, "2001:db8::4", 8.75, true},
                           {id, t + 500, 1, "", -1, false},
                           {id, t + 500, 2, "", -1, false},
                           {id, t + 750, 1, "::1", 1, false}}),
            "Seed incident");
    const QString note = "ISP, \"case 12\"\nfollow-up";
    require(source.addNote(id, t + 275, note) && source.addNote(id, t, "exclude"),
            "Seed incident annotations");
    const auto csv = dir.filePath("incident.csv"), recording = dir.filePath("incident.hopview");
    require(source.exportIncident(id, t + 250, t + 500, csv, true), "Export raw CSV");
    QFile rows(csv);
    require(rows.open(QIODevice::ReadOnly), "Open CSV");
    const auto content = rows.readAll();
    require(content.count("\"probe\"") == 4 && content.count("\"timeout\"") == 2 &&
                content.contains("ISP, \"\"case 12\"\"\nfollow-up") && !content.contains("exclude") &&
                content.contains("2023-11-14T22:13:20.250Z") && content.contains("8443"),
            "CSV preserves raw probes, quoted annotation and settings");
    require(!source.exportIncident(id, t, t + 500, db, false), "Cannot replace own history");
    require(source.exportIncident(id, t + 250, t + 500, recording, false), "Export portable incident");
    IncidentRange range;
    QString error;
    History replay(dir.filePath("replay.db"));
    require(importIncident(recording, replay, range, error), qPrintable(error));
    require(range.from == t + 250 && range.through == t + 500, "Replayed selection boundaries");
    const auto imported = replay.sessions().first().toMap();
    require(imported["target"] == "service.example" && imported["family"] == 6 && imported["port"] == 8443 &&
                imported["protocol"] == "tcp" && imported["interfaceName"] == "eth1" &&
                imported["intervalMs"] == 250,
            "Replay transport settings");
    const auto before = source.view(id, t + 250, t + 500, t + 250, t + 500, {0, 1}, 200);
    const auto after =
        replay.view(imported["id"].toLongLong(), t + 250, t + 500, t + 250, t + 500, {0, 1}, 200);
    require(before == after, "Replay reproduces focus statistics, charts and annotations exactly");
    QFile broken(recording);
    require(broken.open(QIODevice::ReadWrite) && broken.resize(broken.size() - 8), "Truncate fixture");
    broken.close();
    History scratch(dir.filePath("broken.db"));
    require(!importIncident(recording, scratch, range, error) && !error.isEmpty(),
            "Reject incomplete replay");
}

void testManagedCollectors(const QString &path) {
    History history(path);
    require(history.open(), "Create receiver history");
    CollectorManagement receiver(path), desktop(path);
    const auto unpinned = desktop.createKey();
    require(unpinned.split('.').size() == 2 && desktop.nodes().isEmpty(), "Unused keys do not appear as nodes");
    const QString fingerprint(64, 'f');
    receiver.setCertificateFingerprint(fingerprint);
    const auto key = desktop.createKey().split('.');
    require(key.size() == 3 && key[0] == "hv1" && key[1].size() == 64 && key[2] == fingerprint,
            "Key carries the live listener's certificate digest");
    const auto otherKey = desktop.createKey().split('.');
    require(otherKey[1] != key[1], "Distinct enrollment secrets");
    sqlite3 *raw = nullptr;
    require(sqlite3_open(QFile::encodeName(path).constData(), &raw) == SQLITE_OK, "Open key storage");
    {
        hopview_sql::Statement row(raw, "SELECT key_digest FROM managed_collectors");
        while (row.next()) require(row.string(0).size() == 64 && row.string(0) != key[1],
                                   "Keep digests instead of enrollment credentials");
    }
    sqlite3_close(raw);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto id2 = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto first = receiver.enroll(key[1], id, "office", {"office"}, 0, {}, 100);
    const auto site = first["site"].toString();
    require(site == "office-2" && first["revision"] == 1 && first["targets"].isArray()
            && first["targets"].toArray().isEmpty(), "Discover unused node without targets or trace history");
    const auto second = receiver.enroll(otherKey[1], id2, "office", {"office"}, 0, {}, 101);
    require(second["site"] == "office-3", "Separate VMs with identical hostnames and static site labels");
    require(history.sessions(true).isEmpty(), "Idle nodes do not create recordings");
    const auto denied = [&](const QString &secret, const QString &identity) {
        try { receiver.enroll(secret, identity, "other", {}, 0, {}, 105); }
        catch (const std::exception &) { return true; }
        return false;
    };
    require(denied(key[1], id2) && denied(unpinned.section('.', 1, 1), id) && denied(QString(64, 'f'), id),
            "A credential binds to one installation and an installation to one credential");
    const QVariantMap v4{{"target", "127.0.0.1"}, {"family", 4}, {"protocol", "tcp"}};
    const QVariantMap v6{{"target", "::1"}, {"family", 6}, {"protocol", "icmp"}};
    require(!desktop.setTargets(site, 1, {v4,v4}) && !desktop.setTargets(site, 1, {
                QVariantMap{{"target", "127.0.0.1"}, {"family", 6}}}), "Validate full desired list before changing it");
    require(desktop.setTargets(site, 1, {v4, v6}) && !desktop.setTargets(site, 1, {}),
            "Reject outdated edits to the same node");
    require(desktop.setTargets(site, 2, {v4, v6}), "Resaving a canonical list preserves its revision");
    const auto retry = receiver.enroll(key[1], id, "renamed", {}, 1, "invalid interface", 110);
    require(retry["site"] == site && retry["revision"] == 2 && retry["targets"].toArray().size() == 2,
            "Authenticated reconnect keeps the original site and returns outstanding revision");
    auto node = desktop.nodes().first().toMap();
    require(node["applyError"] == "invalid interface" && node["hostname"] == "renamed"
            && node["appliedRevision"] == 1 && node["lastSeenMs"] == 110, "Record per-node status");
    const auto ack = receiver.heartbeat(site, 2, {}, 115);
    require(ack["revision"] == 2 && !ack.contains("targets") && desktop.nodes().first().toMap()["applyError"] == "",
            "Applied revision heartbeat only contains metadata");
    require(desktop.setTargets(site, 2, {}), "Clear target list");
    const auto empty = receiver.heartbeat(site, 2, {}, 120);
    require(empty["revision"] == 3 && empty["targets"].isArray() && empty["targets"].toArray().isEmpty(),
            "Deliver an explicitly empty revision");
}

void benchmark(const QString &file) {
    constexpr qint64 base = 1720012345000;
    constexpr int hops = 15, seconds = 86400;
    History writer(file);
    qint64 id = writer.createSession("benchmark", 4, 1000);
    require(id > 0, "Create benchmark session");
    QVector<Sample> batch;
    QStringList addresses;
    for (int hop = 1; hop <= hops; ++hop)
        addresses.append(QString("192.0.2.%1").arg(hop));
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < seconds; ++i) {
        for (int hop = 1; hop <= hops; ++hop) {
            bool loss = (hop + i) % 101 == 0;
            batch.append({id, base + i * 1000LL, hop, loss ? QString() : addresses[hop - 1],
                          loss ? -1.0 : double(2 * hop + i % 23), hop == hops && !loss});
        }
        if ((i + 1) % 600 == 0) {
            require(writer.append(batch), "Append benchmark data");
            batch.clear();
        }
    }
    auto writeMs = clock.elapsed();
    History reader(file);
    require(reader.open(), "Read benchmark database");
    QList<qint64> elapsed;
    for (int i = 0; i < 11; ++i) {
        clock.restart();
        auto view = reader.view(id, base + i, base + seconds * 1000LL - 1, base + i,
                                base + seconds * 1000LL - 1, {1, 7, 0}, 1200);
        elapsed.append(clock.nsecsElapsed() / 1000);
        require(view["charts"].toList().size() == 3 && view["rows"].toList().size() == 15,
                "Benchmark viewport");
    }
    auto first = elapsed.takeFirst();
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << "24h, 1s interval, 15 hops, " << hops * seconds << " raw probes.\n"
              << "Insert in 600-round batches: " << writeMs << " ms.\n"
              << "Full-day focus and 3 timelines at 1200px: first " << first / 1000.0 << " ms, warm median "
              << elapsed[elapsed.size() / 2] / 1000.0 << " ms.\n"
              << "SQLite database and WAL: "
              << (QFileInfo(file).size() + QFileInfo(file + "-wal").size()) / (1024.0 * 1024.0) << " MiB.\n";
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    try {
        require(dir.isValid(), "Temporary database");
        if (app.arguments().contains("--benchmark"))
            benchmark(dir.filePath("bench.sqlite"));
        else {
            test(dir.filePath("history.sqlite"));
            testIncident(dir);
            testManagedCollectors(dir.filePath("managed.db"));
            testPeriodComparison(dir.filePath("baseline.db"));
            testComparisonEvidence(dir);
            testRouteChange(dir.filePath("routes.sqlite"));
            testRouteEvents(dir.filePath("route-events.sqlite"));
            testDestinationAlerts(dir.filePath("alerts.sqlite"));
            testUpgrade(dir.filePath("old.sqlite"));
            std::cout << "History checks passed.\n";
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
