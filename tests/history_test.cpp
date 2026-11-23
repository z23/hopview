#include "history.h"

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
            testRouteChange(dir.filePath("routes.sqlite"));
            testUpgrade(dir.filePath("old.sqlite"));
            std::cout << "History checks passed.\n";
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
