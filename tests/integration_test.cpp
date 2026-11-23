#include "backend.h"
#include "graph.h"
#include "history.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHostAddress>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr int hops = 12;
constexpr int testRounds = 2100;
const qint64 base = 1700000000000;
const QString example = "edge.example.net";

qint64 fill(const QString &file, int rounds) {
    History writer(file);
    const auto id = writer.createSession(example, 6, 1000);
    QVector<Sample> batch;
    for (int i = 0; i < rounds; ++i) {
        for (int h = 1; h <= hops; ++h) {
            const bool incident = i >= rounds - 170 && i < rounds - 140 && h >= 6;
            const bool lost = (incident && i % 6 == 0) || (h == 3 && i % 10 == 0);
            const double delay = (incident ? 60 : 0) + h * 2.1 + 1.8 * std::abs(std::sin(i * .115 + h));
            QString ip = QString("2001:db8:%1::1").arg(h);
            if (h == 6 && i < rounds - 120)
                ip = "2001:db8:ef::2";
            batch.append(
                {id, base + i * 1000LL, h, lost ? QString() : ip, lost ? -1 : delay, h == hops && !lost});
        }
        if (batch.size() >= 3600 || i == rounds - 1) {
            if (!writer.append(batch))
                return -1;
            batch.clear();
        }
    }
    return id;
}

bool waitFor(const std::function<bool()> &condition, int timeout = 5000) {
    QElapsedTimer time;
    time.start();
    while (!condition() && time.elapsed() < timeout)
        QTest::qWait(3);
    return condition();
}

QVariantMap rowFor(const Backend &b, int hop) {
    for (const auto &row : b.snapshot().value("rows").toList())
        if (row.toMap().value("hop").toInt() == hop)
            return row.toMap();
    return {};
}

QQuickItem *findItem(QQuickItem *parent, const QString &name) {
    if (parent->objectName() == name)
        return parent;
    for (auto *item : parent->childItems())
        if (auto *found = findItem(item, name))
            return found;
    return nullptr;
}

struct Window {
    explicit Window(Backend &b) {
        engine.rootContext()->setContextProperty("backend", &b);
        engine.rootContext()->setContextProperty("themeAccent", QColor("#64d6b2"));
        QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError> &errors) {
            for (auto &e : errors)
                warnings.append(e.toString());
        });
        engine.load(QUrl("qrc:/Main.qml"));
        if (!engine.rootObjects().isEmpty())
            win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    }
    QQmlApplicationEngine engine;
    QQuickWindow *win = nullptr;
    QStringList warnings;
};

bool fresh(Backend &b) {
    const auto snap = b.snapshot();
    return snap.value("sessionId").toLongLong() == b.sessionId() &&
           snap.value("focusStart").toLongLong() == b.focusStart() &&
           snap.value("viewStart").toLongLong() == b.viewStart();
}

class Integration : public QObject {
    Q_OBJECT
  private slots:
    void desktop();
    void replayWhileProbing();
};

void Integration::desktop() {
    QTemporaryDir dir;
    const auto db = dir.filePath("capture.db");
    const qint64 id = fill(db, testRounds);
    QVERIFY(id > 0);
    History other(db);
    const qint64 otherId = other.createSession("another.example.net", 4, 1000);
    QVERIFY(otherId > 0);
    Backend b(db);
    b.selectSession(id);
    Window ui(b);
    QVERIFY(ui.win);
    QVERIFY(waitFor([&] { return ui.win->isExposed() && rowFor(b, hops).value("sent").toInt() > 0; }));
    const qint64 end = b.viewEnd();
    const auto baseline = rowFor(b, 6).value("avg").toDouble();
    b.setFocusSeconds(-1);
    QVERIFY(waitFor([&] { return fresh(b) && rowFor(b, hops).value("sent").toInt() == testRounds; }));
    b.toggleHop(hops);
    QVERIFY(waitFor([&] { return b.snapshot().value("charts").toList().isEmpty(); }));
    b.toggleHop(hops);
    QVERIFY(waitFor(
        [&] { return b.selectedHops().contains(0) && b.snapshot().value("charts").toList().size() == 1; }));
    b.setFocusSeconds(10);
    b.focusAt(end - 155000);
    QVERIFY(waitFor([&] { return fresh(b) && rowFor(b, 6).value("avg").toDouble() > baseline + 50; }));
    QCOMPARE(rowFor(b, 6).value("address").toString(), QString("2001:db8:ef::2"));
    QVERIFY(rowFor(b, hops).value("loss").toDouble() > 0);
    QCOMPARE(rowFor(b, 2).value("loss").toDouble(), 0.0);

    b.toggleHop(6);
    QVERIFY(waitFor([&] { return b.snapshot().value("charts").toList().size() == 2; }));
    const auto previousFocus = b.focusStart();
    b.selectSession(otherId);
    b.selectSession(id);
    QVERIFY(waitFor([&] { return fresh(b) && b.snapshot().value("charts").toList().size() == 2; }));
    QCOMPARE(b.focusStart(), previousFocus);
    QVERIFY(!b.live());
    const qint64 note = b.focusStart() + 1000;
    b.saveNote(note, "Test incident");
    QVERIFY(waitFor([&] { return b.snapshot().value("notes").toList().size() == 1; }));
    b.panBy(-30000);
    QVERIFY(waitFor([&] { return fresh(b) && b.viewEnd() == end - 30000; }));
    b.goLive();
    QVERIFY(
        waitFor([&] { return fresh(b) && b.live() && rowFor(b, 6).value("avg").toDouble() < baseline + 1; }));
    b.setIntervalMs(500);
    QVERIFY(waitFor([&] { return b.sessions().last().toMap().value("intervalMs").toInt() == 500; }));

    // Hit test a real QML chart. A click on a spike changes the upper table's time bounds.
    auto *input = findItem(ui.win->contentItem(), "timelineInput12");
    QVERIFY2(input, "Missing destination timeline input");
    const QPointF local(input->width() * .49, input->height() * .5);
    QTest::mouseDClick(ui.win, Qt::LeftButton, Qt::NoModifier, input->mapToScene(local).toPoint());
    QVERIFY(waitFor([&] { return !b.live() && fresh(b); }));
    QVERIFY(rowFor(b, hops).value("max").toDouble() > 75);
    const auto screenshot = qEnvironmentVariable("HOPLINE_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(ui.win->grabWindow().save(screenshot));
    }
    ui.win->resize(800, 580);
    QTest::qWait(60);
    QVERIFY(ui.win->isExposed());
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
    History persisted(db);
    auto view = persisted.view(id, note - 1, note + 1, note - 1, note + 1, {}, 32);
    QCOMPARE(view.value("notes").toList().first().toMap().value("text").toString(), QString("Test incident"));
}

void Integration::replayWhileProbing() {
    if (!qEnvironmentVariableIsSet("HOPLINE_LIVE_TEST"))
        QSKIP("Set HOPLINE_LIVE_TEST=1 for the dual-stack replay benchmark.");
    QTemporaryDir dir;
    const auto file = dir.filePath("bench.db");
    constexpr int rounds = 25 * 3600;
    const auto ref = fill(file, rounds);
    QVERIFY(ref > 0);
    Backend b(file);
    Window ui(b);
    QVERIFY(ui.win);
    b.startTrace("127.0.0.1", 4, 250);
    QVERIFY(waitFor([&] { return b.sessions().size() == 2 && rowFor(b, 1).value("destination").toBool(); }));
    const qint64 v4 = b.sessionId();
    b.startTrace("::1", 6, 250);
    QVERIFY(waitFor([&] { return b.sessions().size() == 3 && rowFor(b, 1).value("destination").toBool(); }));
    const qint64 v6 = b.sessionId();
    b.selectSession(ref);
    b.setViewSeconds(86400);
    b.setFocusSeconds(3600);
    b.toggleHop(3);
    b.toggleHop(6);
    QVERIFY(waitFor([&] { return fresh(b) && b.snapshot().value("charts").toList().size() == 3; }));
    QVector<double> presentedMs, renderMs;
    for (int i = 0; i < 32; ++i) {
        QElapsedTimer gesture;
        gesture.start();
        b.panBy((i % 2 ? 1 : -1) * 450000);
        b.focusAt(b.viewStart() + (5 + i % 12) * 3600000LL);
        QVERIFY(waitFor([&] { return fresh(b); }));
        QElapsedTimer render;
        render.start();
        const auto frame = ui.win->grabWindow();
        QVERIFY(!frame.isNull());
        renderMs.append(render.nsecsElapsed() / 1e6);
        presentedMs.append(gesture.nsecsElapsed() / 1e6);
    }
    QVERIFY(!renderMs.isEmpty());
    std::sort(presentedMs.begin(), presentedMs.end());
    std::sort(renderMs.begin(), renderMs.end());
    qInfo("25h recording, 24h visible, 12 hops, three timelines, dual-stack live collection: focus-to-frame "
          "median %.1f ms, p95 %.1f ms; synchronous frame capture median %.1f ms, p95 %.1f ms.",
          presentedMs[presentedMs.size() / 2], presentedMs[presentedMs.size() * 95 / 100],
          renderMs[renderMs.size() / 2], renderMs[renderMs.size() * 95 / 100]);
    sqlite3 *db = nullptr;
    QCOMPARE(sqlite3_open_v2(QFile::encodeName(file), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    for (const auto id : {v4, v6}) {
        sqlite3_stmt *q = nullptr;
        const char *sql = "SELECT count(*),sum(rtt>=0),max(gap),avg(gap) FROM "
                          "(SELECT rtt,t-lag(t) OVER (ORDER BY t) gap FROM samples WHERE hop=1 AND session=?)";
        QCOMPARE(sqlite3_prepare_v2(db, sql, -1, &q, nullptr), SQLITE_OK);
        sqlite3_bind_int64(q, 1, id);
        QCOMPARE(sqlite3_step(q), SQLITE_ROW);
        int count = sqlite3_column_int(q, 0), received = sqlite3_column_int(q, 1);
        qInfo("%s loopback: %d/%d replies, round spacing mean %.1f ms, max %.0f ms (requested 250 ms).",
              id == v4 ? "IPv4" : "IPv6", received, count, sqlite3_column_double(q, 3),
              sqlite3_column_double(q, 2));
        sqlite3_finalize(q);
        QVERIFY(count > 4);
        QCOMPARE(count, received);
    }
    sqlite3_close(db);
    b.selectSession(v6);
    b.toggleRunning();
    QVERIFY(!b.running());
    QTest::qWait(500);
    b.goLive();
    QVERIFY(waitFor([&] { return fresh(b); }));
    const int paused = rowFor(b, 1).value("sent").toInt();
    QTest::qWait(600);
    QCOMPARE(rowFor(b, 1).value("sent").toInt(), paused);
    b.toggleRunning();
    QVERIFY(waitFor([&] { return b.running() && rowFor(b, 1).value("sent").toInt() > paused; }));
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}
} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<GraphView>("Hopline.Graphics", 1, 0, "GraphView");
    Integration suite;
    return QTest::qExec(&suite, argc, argv);
}
#include "integration_test.moc"
