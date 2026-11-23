#include "backend.h"
#include "graph.h"
#include "history.h"
#include "diagnostics.h"
#include <QDateTime>
#include "incident.h"
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
            const double delay = (incident ? 210 : 0) + h * 2.1 + 1.8 * std::abs(std::sin(i * .115 + h));
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
    void alertControls();
    void diagnosticTracks();
    void diagnosticLoopback();
    void probeSettings();
    void routeHistory();
    void latencyColors();
    void tabControlsAndScale();
    void rangeSelection();
    void compareTargets();
    void baselineComparison();
    void incomingSites();
    void twoSiteWindow();
    void savedComparison();
    void saveFocus();
    void replayWhileProbing();
};

void Integration::diagnosticTracks() {
    QTemporaryDir dir;
    const auto path = dir.filePath("diagnostics.sqlite");
    const auto id = fill(path, 100);
    History h(path);
    InterfaceReading first{"test0", "00:11:22:33:44:55", 100, 5, 1, 1000, 100, 0, 0, 0, 0, true};
    auto latest = first;
    latest.tick += 2000;
    latest.rx += 9000;
    latest.tx += 600;
    latest.rxErrors += 3;
    latest.ap = "00:11:22:33:44:66";
    auto nic = interfaceSample(latest, &first, base + 2000);
    QCOMPARE(nic["rx"].toDouble(), 4500.);
    QCOMPARE(nic["tx"].toDouble(), 300.);
    QCOMPARE(nic["rxErrors"].toInt(), 3);
    QVERIFY(nic["event"].toString().contains("Wi-Fi"));
    latest.rx = 10;
    QVERIFY(interfaceSample(latest, &first, base)["rx"].toDouble() < 0);
    latest.tick += 10000;
    QVERIFY(interfaceSample(latest, &first, base)["tx"].toDouble() < 0);
    QVERIFY(h.appendDiagnostic(id, nic));
    QVERIFY(h.appendDiagnostic(id, nic));
    auto service = QVariantMap{{"kind", "service"},   {"url", "https://service.test/"},
                               {"t", base + 22000},   {"address", "2001:db8::1"},
                               {"family", 6},         {"periodMs", 5000},
                               {"dns", 2.},           {"connect", 4.},
                               {"tls", 6.},           {"firstByte", 90.},
                               {"total", 102.},       {"status", 503},
                               {"error", "HTTP 503"}, {"event", "HTTP 503"}};
    QVERIFY(h.appendDiagnostic(id, service));
    service["total"] = 103.;
    QVERIFY(!h.appendDiagnostic(id, service));
    service["t"] = base + 24000;
    service["status"] = 200;
    service["error"] = service["event"] = "";
    QVERIFY(h.appendDiagnostic(id, service));
    // A broad viewport uses the stored peaks and retains a lower peak's event.
    auto longView = h.diagnosticTracks(id, base - 86400000, base + 86400000, 12);
    QCOMPARE(longView.size(), 2);
    auto peak = longView.first().toMap()["points"].toList().last().toMap();
    QCOMPARE(peak["total"].toDouble(), 103.);
    QCOMPARE(peak["marker"].toMap()["status"].toInt(), 503);
    auto narrow = h.diagnosticTracks(id, base + 22001, base + 24000, 100);
    QCOMPARE(narrow.size(), 1);
    QCOMPARE(narrow.first().toMap()["points"].toList().size(), 1);
    QVERIFY(!narrow.first().toMap()["points"].toList().first().toMap().contains("marker"));

    const auto control = h.createSession("control.example", 4, 1000);
    QVERIFY(h.append({{control, base + 22000, 1, "192.0.2.1", 3., true}}));
    auto controlNic = nic;
    controlNic["name"] = "control0";
    QVERIFY(h.appendDiagnostic(control, controlNic));
    const auto evidence = dir.filePath("multiple.hopline");
    QVERIFY2(h.exportIncident(id, base, base + 99000, evidence, false, {control}), qPrintable(h.error()));
    History imported(dir.filePath("imported.db"));
    IncidentRange clip;
    QString error;
    QVERIFY2(importIncident(evidence, imported, clip, error), qPrintable(error));
    QCOMPARE(clip.sessions.size(), 2);
    for (int i = 0; i < 2; ++i) {
        const auto originalId = i ? control : id;
        QCOMPARE(imported.diagnosticTracks(clip.sessions[i], base, base + 99000, 256),
                 h.diagnosticTracks(originalId, base, base + 99000, 256));
    }
    QVERIFY(imported.diagnosticsSettings().isEmpty());
    Backend b(path);
    b.selectSession(id);
    b.zoomToRange(base, base + 99000);
    Window ui(b);
    QVERIFY(ui.win &&
            waitFor([&] { return findItem(ui.win->contentItem(), "serviceDiagnosticGraph") && fresh(b); }));
    const auto screenshot = qEnvironmentVariable("HOPLINE_DIAGNOSTIC_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        QTest::qWait(70);
        QVERIFY(ui.win->grabWindow().save(screenshot));
    }
    ui.win->resize(ui.win->minimumWidth(), ui.win->minimumHeight());
    QTest::qWait(70);
    auto *graph = findItem(ui.win->contentItem(), "serviceDiagnosticGraph");
    // The tracks scroll vertically at the minimum window size.
    QVERIFY(graph && graph->width() > 100);
    ui.win->resize(1220, 800);
    QTest::qWait(50);
    auto mousePosition = [&](double fraction) {
        return graph->mapToScene({graph->width() * fraction, graph->height() / 2}).toPoint();
    };
    QTest::mousePress(ui.win, Qt::LeftButton, Qt::NoModifier, mousePosition(.17));
    QTest::mouseMove(ui.win, mousePosition(.31));
    QTest::mouseRelease(ui.win, Qt::LeftButton, Qt::NoModifier, mousePosition(.31));
    QVERIFY(waitFor([&] { return fresh(b) && b.viewSeconds() < 18; }));
    auto *settings = ui.win->findChild<QObject *>("diagnosticSettings");
    QVERIFY(settings && QMetaObject::invokeMethod(settings, "open"));
    auto *url = findItem(ui.win->contentItem(), "serviceUrl");
    auto *save = findItem(ui.win->contentItem(), "saveDiagnostics");
    QVERIFY(url && save && url->setProperty("text", "file:///tmp/invalid"));
    QVERIFY(QMetaObject::invokeMethod(save, "click"));
    QVERIFY(waitFor([&] { return !settings->property("problem").toString().isEmpty(); }));
    QVERIFY(url->setProperty("text", "https://service.test/"));
    QVERIFY(QMetaObject::invokeMethod(save, "click"));
    QVERIFY(waitFor([&] { return !settings->property("visible").toBool(); }));
    QCOMPARE(h.diagnosticsSettings()[QString::number(id)].toMap()["url"].toString(),
             QString("https://service.test/"));
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::diagnosticLoopback() {
    if (!qEnvironmentVariableIsSet("HOPLINE_DIAG_TLS"))
        QSKIP("Run python3 tests/diagnostics-loopback.py to start private HTTP and HTTPS fixtures.");
    const auto https = qEnvironmentVariable("HOPLINE_DIAG_TLS");
    const auto http = qEnvironmentVariable("HOPLINE_DIAG_HTTP");
    const auto https6 = qEnvironmentVariable("HOPLINE_DIAG_TLS6");
    const auto ca = qgetenv("HOPLINE_DIAG_CA");
    const std::atomic_bool cancel{false};
    for (const int family : {4, 6}) {
        const auto record = measureService(family == 4 ? https : https6, family, "", 5, cancel, ca);
        QVERIFY2(record["error"].toString().isEmpty(), qPrintable(record["error"].toString()));
        QCOMPARE(record["status"].toInt(), 200);
        QCOMPARE(record["family"].toInt(), family);
        QVERIFY(!record["address"].toString().isEmpty());
        for (const auto key : {"dns", "connect", "tls", "firstByte", "total"})
            QVERIFY(record[key].toDouble() >= 0);
        QVERIFY(record["firstByte"].toDouble() >= 20);
        QVERIFY(record["total"].toDouble() >= record["firstByte"].toDouble());
    }
    auto untrusted = measureService(https, 4, "", 5, cancel);
    QVERIFY(!untrusted["error"].toString().isEmpty() && untrusted["tls"].toDouble() < 0);
    QCOMPARE(untrusted["status"].toInt(), 0);
    auto failure = measureService(http + "failure", 4, "", 5, cancel);
    QCOMPARE(failure["status"].toInt(), 503);
    QCOMPARE(failure["error"].toString(), QString("HTTP 503"));
    QCOMPARE(failure["tls"].toDouble(), -1.);
    QCOMPARE(failure["dns"].toDouble(), -1.);

    QTemporaryDir temp;
    const auto path = temp.filePath("collect.sqlite");
    History storage(path);
    const auto id = storage.createSession("127.0.0.1", 4, 250);
    Backend b(path);
    b.setDiagnostics(id, http, "lo", 5);
    QVERIFY(waitFor([&] { return !b.diagnosticsSettings().isEmpty(); }));
    b.toggleRunning();
    auto observations = [&] {
        QVariantList rows;
        storage.eachDiagnostic(id, 0, QDateTime::currentMSecsSinceEpoch(),
                               [&](const QVariantMap &r) { rows << r; });
        return rows;
    };
    auto completed = [&] {
        bool service = false, interface = false;
        for (const auto &v : observations()) {
            const auto r = v.toMap();
            service |= r["kind"] == "service" && r["status"].toInt() == 200;
            interface |= r["kind"] == "interface" && r["rx"].toDouble() > 0;
        }
        return service && interface;
    };
    QVERIFY(waitFor(completed, 12000));
    b.toggleRunning();
    QTest::qWait(200);
    auto paused = observations().size();
    QTest::qWait(2300);
    QCOMPARE(observations().size(), paused);
}

void Integration::probeSettings() {
    QTemporaryDir dir;
    const auto db = dir.filePath("protocol.db");
    Backend backend(db);
    Window ui(backend);
    QVERIFY(ui.win && waitFor([&] { return ui.win->isExposed(); }));
    auto *controls = findItem(ui.win->contentItem(), "probeControls");
    QVERIFY(controls);
    QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                      controls->mapToScene(controls->boundingRect().center()).toPoint());
    QVERIFY(waitFor([&] { return findItem(ui.win->contentItem(), "probePort"); }));
    auto *method = findItem(ui.win->contentItem(), "probeProtocol");
    auto *port = findItem(ui.win->contentItem(), "probePort");
    auto *iface = findItem(ui.win->contentItem(), "probeInterface");
    QVERIFY(method && port && iface);
    QVERIFY(method->setProperty("currentIndex", 3));
    QVERIFY(QMetaObject::invokeMethod(method, "activated", Q_ARG(int, 3)));
    QCOMPARE(port->property("value").toInt(), 443);
    QVERIFY(port->setProperty("value", 8443));
    QString expectedInterface;
    const int loopback = backend.interfaceNames().indexOf("lo");
    if (loopback >= 0) {
        QVERIFY(iface->setProperty("currentIndex", loopback + 1));
        expectedInterface = "lo";
        QCOMPARE(iface->property("currentText").toString(), expectedInterface);
    }
    QTest::keyClick(ui.win, Qt::Key_Escape);
    auto *target = findItem(ui.win->contentItem(), "destinationInput");
    QVERIFY(target && target->setProperty("text", "127.0.0.1"));
    QVERIFY(QMetaObject::invokeMethod(target, "accepted"));
    QVERIFY(waitFor([&] { return !backend.sessions().isEmpty(); }));
    const auto saved = History(db).sessions().first().toMap();
    QCOMPARE(saved["protocol"].toString(), QString("tcp"));
    QCOMPARE(saved["port"].toInt(), 8443);
    QCOMPARE(saved["interfaceName"].toString(), expectedInterface);
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::alertControls() {
    QTemporaryDir dir;
    const auto db = dir.filePath("alerts.db");
    History h(db);
    auto id = h.createSession("alert.test", 4, 1000);
    QVERIFY(h.setAlertRule(id, 100, 50, 1));
    for (int i = 0; i < 4; ++i)
        QVERIFY(h.append({{id, base + i * 1000, 1, "target", 200, true}}));
    QVERIFY(h.setTabClosed(id, true));
    auto open = h.createSession("another.test", 6, 500);
    Backend b(db);
    b.enableRecorder();
    Window ui(b);
    QVERIFY(ui.win && waitFor([&] { return ui.win->isExposed(); }));
    QCOMPARE(b.sessionId(), open);
    QCOMPARE(b.alertEvents().size(), 1);
    auto *dialog = ui.win->findChild<QObject *>("alertsDialog");
    QVERIFY(dialog && QMetaObject::invokeMethod(dialog, "open"));
    QVERIFY(waitFor([&] { return dialog->property("opened").toBool() && findItem(ui.win->contentItem(), "keepRecording"); }));
    QSignalSpy rendered(ui.win, &QQuickWindow::frameSwapped);
    ui.win->requestUpdate();
    QVERIFY(waitFor([&] { return !rendered.isEmpty(); }));
    auto click = [&](const char *name) {
        auto *item = findItem(ui.win->contentItem(), name);
        if (!item) return false;
        QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        return true;
    };
    QVERIFY(click("keepRecording"));
    QVERIFY(waitFor([&] { return b.backgroundEnabled(); }));
    QVERIFY(h.monitoringSettings()["background"].toBool());
    dialog->setProperty("loss", 25);
    QVERIFY(click("saveAlertThresholds"));
    QVERIFY(waitFor([&] { return b.alertRules().contains(QString::number(open)); }));
    QCOMPARE(h.monitoringSettings()["rules"].toMap()[QString::number(open)].toMap()["loss"].toInt(), 25);
    // The alarm belongs to an archive. Its event opens precisely the historical
    // evidence without reactivating the collector.
    QVERIFY(click("alertEvent1"));
    QVERIFY(waitFor([&] { return b.sessionId() == id && fresh(b); }));
    QVERIFY(!b.live() && !b.running());
    QCOMPARE(b.focusStart(), base);
    QCOMPARE(b.focusEnd(), base + 3000);
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

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

void Integration::tabControlsAndScale() {
    QTemporaryDir dir;
    const auto db = dir.filePath("tabs.db");
    const auto first = fill(db, 120);
    History recorder(db);
    const auto second = recorder.createSession(example, 6, 1000);
    QVERIFY(second > first && first > 0);
    {
        Backend b(db);
        Window ui(b);
        QVERIFY(ui.win);
        QVERIFY(waitFor([&] { return ui.win->isExposed(); }));
        ui.win->requestActivate();
        const auto scale = [&] { return ui.win->property("uiScale").toReal(); };
        QTest::keyClick(ui.win, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(scale(), 1.0);
        QTest::keyClick(ui.win, Qt::Key_Plus, Qt::ControlModifier);
        QVERIFY(waitFor([&] { return scale() > 1.0; }));
        const double raised = scale();
        auto *workspace = findItem(ui.win->contentItem(), "workspace");
        QVERIFY(workspace);
        QCOMPARE(workspace->scale(), raised);
        QTest::keyClick(ui.win, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(scale() > raised);
        QTest::keyClick(ui.win, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(scale(), raised);
        QTest::keyClick(ui.win, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(scale(), 1.0);
        QTest::keyClick(ui.win, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(scale(), raised);
        auto clickControl = [&](QString name, Qt::MouseButton button) {
            auto *item = findItem(ui.win->contentItem(), name);
            QVERIFY2(item, qPrintable(name));
            QTest::mouseClick(ui.win, button, Qt::NoModifier,
                              item->mapToScene(item->boundingRect().center()).toPoint());
        };
        // Close the background tab with middle-click and the selected tab with its X.
        clickControl("sessionTab" + QString::number(first), Qt::MiddleButton);
        QVERIFY(waitFor([&] { return b.closedSessions().size() == 1; }));
        QCOMPARE(b.sessionId(), second);
        clickControl("closeTab" + QString::number(second), Qt::LeftButton);
        QVERIFY(waitFor([&] { return b.sessions().isEmpty() && !b.sessionId(); }));
        QVERIFY(b.snapshot().isEmpty());
        QCOMPARE(b.closedSessions().size(), 2);
        QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
    }
    {
        Backend b(db);
        Window ui(b);
        QVERIFY(ui.win);
        QVERIFY(waitFor([&] { return ui.win->isExposed(); }));
        QVERIFY(ui.win->property("uiScale").toReal() > 1.0);
        QVERIFY(b.sessions().isEmpty());
        QCOMPARE(b.closedSessions().size(), 2);
        auto *button = findItem(ui.win->contentItem(), "reopenTraces");
        QVERIFY(button && button->isVisible());
        QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                          button->mapToScene(button->boundingRect().center()).toPoint());
        QQuickItem *entry = nullptr;
        QVERIFY(waitFor([&] {
            entry = findItem(ui.win->contentItem(), "reopenTrace" + QString::number(first));
            return entry && entry->isVisible() && entry->width() > 0;
        }));
        QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                          entry->mapToScene(entry->boundingRect().center()).toPoint());
        QVERIFY(waitFor([&] { return b.sessionId() == first && rowFor(b, hops).value("sent").toInt() > 0; }));
        QVERIFY(!b.running());
        QTest::keyClick(ui.win, Qt::Key_0, Qt::ControlModifier);
        QCOMPARE(ui.win->property("uiScale").toReal(), 1.0);
        QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
    }
}

void Integration::routeHistory() {
    QTemporaryDir dir;
    auto db = dir.filePath("routes.db");
    QVERIFY(fill(db, 150) > 0);
    Backend b(db);
    b.setReplayMode();
    Window ui(b);
    QVERIFY(ui.win);
    auto *pause = findItem(ui.win->contentItem(), "pauseButton");
    QVERIFY(pause && !pause->isVisible() && !b.running());
    auto *history = ui.win->findChild<QObject *>("routeHistoryDialog");
    QVERIFY(history);
    QVERIFY(QMetaObject::invokeMethod(history, "open"));
    QVERIFY(waitFor([&] { return history->property("eventCount").toInt() == 1 &&
                                !history->property("scanning").toBool(); }));
    auto *path = findItem(ui.win->contentItem(), "routePath");
    QVERIFY(path);
    QCOMPARE(path->property("count").toInt(), hops);
    auto *show = findItem(ui.win->contentItem(), "showRouteEvent");
    QVERIFY(show && show->isEnabled());
    QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                      show->mapToScene({show->width()/2, show->height()/2}).toPoint());
    QVERIFY(waitFor([&] { return !b.live() && fresh(b); }));
    QCOMPARE(b.viewStart(), base + 27000);
    QCOMPARE(b.viewEnd(), base + 32000);
    QVERIFY(!history->property("visible").toBool());
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::baselineComparison() {
    QTemporaryDir dir;
    const auto db = dir.filePath("baseline.sqlite");
    QVERIFY(fill(db, 150) > 0);
    Backend b(db);
    Window ui(b);
    QVERIFY(ui.win);
    ui.win->resize(ui.win->minimumSize());
    QVERIFY(waitFor([&] { return ui.win->isExposed(); }));
    ui.win->requestActivate();
    QTest::keyClick(ui.win, Qt::Key_0, Qt::ControlModifier);
    for (int i = 0; i < 8; ++i)
        QTest::keyClick(ui.win, Qt::Key_Plus, Qt::ControlModifier);
    QCOMPARE(ui.win->property("uiScale").toReal(), 2.0);
    b.zoomToRange(base, base + 10000);
    b.markBaseline();
    b.zoomToRange(base + 20000, base + 30000);
    auto *dialog = ui.win->findChild<QObject *>("baselineDialog");
    QVERIFY(dialog);
    QSignalSpy response(&b, &Backend::comparisonReady);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QVERIFY(waitFor(
        [&] { return !response.isEmpty() && dialog->property("results").toList().size() == hops + 1; }));
    const auto result = response.takeFirst();
    QCOMPARE(result[1].toLongLong(), base);
    QCOMPARE(result[2].toLongLong(), base + 10000);
    QCOMPARE(result[3].toLongLong(), base + 20000);
    QCOMPARE(result[4].toLongLong(), base + 30000);
    QVERIFY(dialog->property("compact").toBool());
    auto *list = findItem(ui.win->contentItem(), "periodRows");
    QVERIFY(list && list->height() > 0);
    QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 2)));
    QVERIFY(waitFor([&] {
        auto *value = findItem(ui.win->contentItem(), "compactIncident0_4");
        if (!value || !value->isVisible() || value->width() < value->implicitWidth())
            return false;
        const auto center = value->mapToItem(list, value->boundingRect().center());
        return list->boundingRect().contains(center) &&
               QRect(QPoint(), ui.win->size())
                   .contains(value->mapToScene(value->boundingRect().center()).toPoint());
    }));
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QTest::keyClick(ui.win, Qt::Key_0, Qt::ControlModifier);
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::twoSiteWindow() {
    const auto db = qEnvironmentVariable("HOPLINE_REMOTE_DB");
    if (db.isEmpty())
        QSKIP("Run tests/telemetry.py with --viewer for two live remote senders.");
    Backend b(db);
    Window ui(b);
    QVERIFY(ui.win && waitFor([&] { return ui.win->isExposed(); }));
    QFile ready(QFileInfo(db).dir().filePath("ui.ready"));
    QVERIFY(ready.open(QIODevice::WriteOnly));
    ready.close();
    qint64 office = 0, branch = 0;
    QVERIFY(waitFor(
        [&] {
            for (const auto &value : b.sessions()) {
                const auto trace = value.toMap();
                if (trace["family"].toInt() != 4 || !trace["remoteLive"].toBool() ||
                    trace["clockErrorMs"].toInt() < 0)
                    continue;
                if (trace["location"] == "office")
                    office = trace["id"].toLongLong();
                if (trace["location"] == "branch")
                    branch = trace["id"].toLongLong();
            }
            return office && branch;
        },
        45000));
    b.selectSession(office);
    b.toggleComparison(branch);
    QVERIFY(waitFor([&] {
        const auto charts = b.snapshot()["charts"].toList();
        return charts.size() == 2 && charts.last().toMap()["location"] == "branch" &&
               !charts.first().toMap()["buckets"].toList().isEmpty() &&
               !charts.last().toMap()["buckets"].toList().isEmpty();
    }));
    const auto received = b.viewEnd();
    QVERIFY(waitFor([&] { return b.viewEnd() > received; }));
    QVERIFY(!ui.win->grabWindow().isNull());
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::incomingSites() {
    QTemporaryDir dir;
    const auto db=dir.filePath("sites.sqlite");
    Backend b(db);
    Window ui(b);
    QVERIFY(ui.win);
    History receiver(db);
    const auto a=receiver.receiverSession("london","r1","service.example",4,1000,"tcp",443,"");
    const auto c=receiver.receiverSession("melbourne","r2","service.example",4,1000,"tcp",443,"");
    QVERIFY(a>0 && c>0);
    QVERIFY(receiver.append({{a,base,1,"192.0.2.1",5,true},{c,base,1,"192.0.2.1",90,true}}));
    QVERIFY(receiver.setSessionOrigin(a,"london","192.0.2.1",5,2,base+100));
    QVERIFY(receiver.setSessionOrigin(c,"melbourne","192.0.2.1",-8,3,base+100));
    QVERIFY(waitFor([&]{return b.sessions().size()==2 && b.sessionId()>0;}));
    b.selectSession(a);
    b.toggleRunning();
    QVERIFY(!b.running());
    b.toggleComparison(c);
    b.zoomToRange(base-1000,base);
    QVERIFY(waitFor([&]{return fresh(b) && b.snapshot()["charts"].toList().size()==2;}));
    const auto peer=b.snapshot()["charts"].toList().last().toMap();
    QCOMPARE(peer["location"].toString(),QString("melbourne"));
    QCOMPARE(peer["clockErrorMs"].toInt(),3);
    auto *pause=findItem(ui.win->contentItem(),"pauseButton");
    QVERIFY(pause && !pause->isVisible());
    // A delayed batch changes a historical window without moving its pinned bounds.
    QVERIFY(receiver.append({{c,base-500,1,"192.0.2.1",120,true}}));
    QVERIFY(receiver.setSessionOrigin(c,"melbourne","192.0.2.1",-8,3,base+3000));
    QVERIFY(waitFor([&]{return b.snapshot()["timeMax"].toDouble()==120;}));
    QCOMPARE(b.viewEnd(),base);
    QVERIFY2(ui.warnings.isEmpty(),qPrintable(ui.warnings.join('\n')));
}

void Integration::saveFocus() {
    QTemporaryDir dir;
    const auto file = dir.filePath("source.db");
    const auto id = fill(file, 150);
    Backend b(file);
    const auto start = base + 9500, end = base + 14500;
    b.zoomToRange(start, end);
    Window ui(b);
    QVERIFY(ui.win);
    QVERIFY(waitFor([&] { return ui.win->isExposed() && fresh(b); }));
    auto *dialog = ui.win->findChild<QObject *>("incidentExportDialog");
    QVERIFY(dialog && dialog->setProperty("currentFolder", QUrl::fromLocalFile(dir.path())));
    auto *menu = findItem(ui.win->contentItem(), "traceActions");
    QVERIFY(menu);
    QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                      menu->mapToScene(menu->boundingRect().center()).toPoint());
    auto *save = findItem(ui.win->contentItem(), "saveIncidentAction");
    QVERIFY(save && save->isVisible());
    QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                      save->mapToScene(save->boundingRect().center()).toPoint());
    QVERIFY(waitFor([&] { return dialog->property("visible").toBool(); }));
    QCOMPARE(dialog->property("fromMs").toLongLong(), start);
    QCOMPARE(dialog->property("throughMs").toLongLong(), end);
    QCOMPARE(dialog->property("session").toLongLong(), id);
    b.goLive(); // The export must keep the range captured when the dialog opened.
    const auto path = dir.filePath("selected.hopline");
    QSignalSpy result(&b, &Backend::incidentExported);
    QQuickItem *name = nullptr;
    QVERIFY(waitFor([&] {
        for (auto *window : QGuiApplication::allWindows()) {
            auto *quick = qobject_cast<QQuickWindow *>(window);
            if (quick && (name = findItem(quick->contentItem(), "fileNameTextField")) && name->isVisible())
                return true;
        }
        return false;
    }));
    auto *picker = name->window();
    name->forceActiveFocus();
    QTest::keyClick(name->window(), Qt::Key_A, Qt::ControlModifier);
    QInputMethodEvent filename;
    filename.setCommitString("selected.hopline");
    QGuiApplication::sendEvent(name, &filename);
    QCOMPARE(name->property("text").toString(), QString("selected.hopline"));
    // Confirm using the file picker's Save button.
    QQuickItem *confirm = nullptr;
    std::function<void(QQuickItem *)> findSave = [&](QQuickItem *control) {
        if (control->property("text").toString().remove('&') == "Save" && control->isVisible() &&
            control->metaObject()->indexOfSignal("clicked()") >= 0)
            confirm = control;
        for (auto *child : control->childItems())
            findSave(child);
    };
    findSave(picker->contentItem());
    QVERIFY(confirm && confirm->isEnabled());
    QTest::mouseClick(picker, Qt::LeftButton, Qt::NoModifier,
                      confirm->mapToScene(confirm->boundingRect().center()).toPoint());
    QVERIFY(waitFor([&] { return !result.isEmpty(); }));
    const auto outcome = result.takeFirst();
    QVERIFY2(outcome[0].toBool(), qPrintable(outcome[1].toString()));
    IncidentRange saved;
    History replay(dir.filePath("replay.db"));
    QString error;
    QVERIFY2(importIncident(path, replay, saved, error), qPrintable(error));
    QCOMPARE(saved.from, start);
    QCOMPARE(saved.through, end);
    const auto session = replay.sessions().first().toMap();
    QCOMPARE(session["startMs"].toLongLong(), base + 10000);
    QCOMPARE(session["endMs"].toLongLong(), base + 14000);
}

void Integration::savedComparison() {
    QTemporaryDir dir;
    const auto file = dir.filePath("groups.db");
    const auto incident = dir.filePath("office.hopline");
    const auto primary = fill(file, 80);
    History writer(file);
    const auto peer = writer.createSession("isp.example", 4, 1000);
    QVERIFY(writer.append({{peer, base + 1000, 1, "192.0.2.3", 9, true}}));
    {
        Backend b(file);
        b.selectSession(primary);
        b.zoomToRange(base+500,base+4200);
        Window ui(b);
        QVERIFY(ui.win && waitFor([&] { return ui.win->isExposed(); }));
        auto item = [&](const QString &key) { return findItem(ui.win->contentItem(), key); };
        auto click = [&](QQuickItem *control) {
            QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                              control->mapToScene(control->boundingRect().center()).toPoint());
        };
        QVERIFY(item("compareTargets"));
        click(item("compareTargets"));
        const auto choice = "compareChoice" + QString::number(peer);
        QVERIFY(waitFor([&] { return item(choice) && item(choice)->isVisible(); }));
        click(item(choice));
        QVERIFY(waitFor([&] { return item("groupName") && item("groupName")->isVisible(); }));
        QVERIFY(item("groupName")->setProperty("text", "Gateway, ISP and service"));
        click(item("saveGroup"));
        QVERIFY(waitFor([&] { return b.groupNames().size() == 1; }));
        QTest::keyClick(ui.win, Qt::Key_Escape);
        b.closeSession(peer);
        QVERIFY(waitFor([&] { return b.sessions().size() == 1; }));
        QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
    }
    Backend b(file);
    b.openGroup("Gateway, ISP and service");
    QVERIFY(waitFor([&] { return b.sessionId() == primary && b.comparisonIds() == QVariantList{peer}; }));
    QVERIFY(!b.running() && !b.live() && b.sessions().size() == 2);
    QCOMPARE(b.viewStart(),base+500);
    QCOMPARE(b.viewEnd(),base+4200);
    QSignalSpy saved(&b, &Backend::incidentExported);
    b.exportIncident(primary, base, base + 3000, QUrl::fromLocalFile(incident), false, {peer}, {0, 3});
    QVERIFY(waitFor([&] { return !saved.isEmpty(); }));
    QVERIFY(saved.first()[0].toBool());
    QSignalSpy added(&b, &Backend::incidentAdded);
    b.addIncident(QUrl::fromLocalFile(incident), "Offsite", 500);
    QVERIFY(waitFor([&] { return !added.isEmpty(); }));
    QVERIFY2(added.first()[0].toBool(), qPrintable(added.first()[1].toString()));
    QCOMPARE(b.sessionId(), primary);
    QCOMPARE(b.viewStart(), base + 500);
    QCOMPARE(b.viewEnd(), base + 3500);
    QCOMPARE(b.comparisonIds().size(), 3);
    QCOMPARE(b.sessions().size(), 4);
    const auto imported = b.comparisonIds().last().toLongLong();
    b.selectSession(imported);
    b.toggleRunning();
    QVERIFY(!b.running());
    Window ui(b);
    QVERIFY(ui.win);
    auto *pause = findItem(ui.win->contentItem(), "pauseButton");
    QVERIFY(pause && !pause->isVisible());
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::compareTargets() {
    QTemporaryDir dir;
    const auto file = dir.filePath("compare.db");
    const auto id = fill(file, 150);
    History writer(file);
    const auto peer = writer.createSession("192.0.2.1", 4, 1000);
    QVector<Sample> samples;
    for (int i = 50; i < 150; ++i)
        samples.append({peer, base + i * 1000, 1, "192.0.2.1", 83, true});
    QVERIFY(writer.append(samples));
    Backend b(file);
    b.selectSession(id);
    b.zoomToRange(base, base + 149000);
    Window ui(b);
    QVERIFY(ui.win);
    QVERIFY(waitFor([&] { return ui.win->isExposed() && fresh(b); }));
    auto item = [&](const QString &name) { return findItem(ui.win->contentItem(), name); };
    auto click = [&](QQuickItem *control) {
        QTest::mouseClick(ui.win, Qt::LeftButton, Qt::NoModifier,
                          control->mapToScene(control->boundingRect().center()).toPoint());
    };
    QVERIFY(item("compareTargets"));
    click(item("compareTargets"));
    const auto peerKey = QString::number(peer);
    QVERIFY(waitFor([&] { return item("compareChoice" + peerKey) != nullptr; }));
    click(item("compareChoice" + peerKey));
    QVERIFY(waitFor(
        [&] { return b.comparisonIds().contains(peer) && b.snapshot()["charts"].toList().size() == 2; }));
    QTest::keyClick(ui.win, Qt::Key_Escape);
    QVERIFY(waitFor([&] {
        auto *dialog = ui.win->findChild<QObject *>("compareDialog");
        return dialog && !dialog->property("visible").toBool();
    }));
    auto *mouse = item("compareInput" + peerKey);
    QVERIFY(mouse);
    const auto point = [&](qreal fraction) {
        return mouse->mapToScene({fraction * mouse->width(), mouse->height() / 2}).toPoint();
    };
    const auto readout = [&](const QString &key) {
        auto *label = item("cursorValue" + key);
        return label ? label->property("text").toString() : QString{};
    };
    QTest::mouseMove(ui.win, point(0.2));
    QVERIFY(waitFor(
        [&] { return readout("peer" + peerKey) == "No samples" && readout("hop12").contains("ms"); }));
    QTest::mouseMove(ui.win, point(0.75));
    QVERIFY(waitFor(
        [&] { return readout("peer" + peerKey).contains("83 ms") && readout("hop12").contains("ms"); }));
    QTest::mousePress(ui.win, Qt::LeftButton, Qt::NoModifier, point(0.5));
    QTest::mouseMove(ui.win, point(0.8), 30);
    QTest::mouseRelease(ui.win, Qt::LeftButton, Qt::NoModifier, point(0.8));
    QVERIFY(waitFor([&] { return fresh(b) && b.viewSeconds() < 46 && b.viewSeconds() > 44; }));
    QCOMPARE(b.focusStart(), b.viewStart());
    const auto charts = b.snapshot()["charts"].toList();
    QCOMPARE(charts.size(), 2);
    QCOMPARE(charts[1].toMap()["target"].toString(), QString("192.0.2.1"));
    QCOMPARE(charts[1].toMap()["buckets"].toList().size(), charts[0].toMap()["buckets"].toList().size());
    b.selectSession(peer);
    QVERIFY(b.comparisonIds().isEmpty());
    b.selectSession(id);
    QCOMPARE(b.comparisonIds().size(), 1);
    b.closeSession(peer);
    QVERIFY(
        waitFor([&] { return b.comparisonIds().isEmpty() && b.snapshot()["charts"].toList().size() == 1; }));
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::rangeSelection() {
    QTemporaryDir dir;
    const auto db = dir.filePath("zoom.db");
    const auto id = fill(db, testRounds);
    Backend b(db);
    Window ui(b);
    QVERIFY(ui.win);
    QVERIFY(waitFor([&] { return ui.win->isExposed() && fresh(b) && !rowFor(b, hops).isEmpty(); }));
    b.toggleHop(6);
    QVERIFY(waitFor([&] { return b.snapshot().value("charts").toList().size() == 2; }));
    const auto initial = b.viewEnd() - b.viewStart();
    for (bool reverse : {false, true}) {
        auto *plot = findItem(ui.win->contentItem(), "timelineInput" + QString::number(reverse ? 6 : hops));
        QVERIFY(plot && plot->isVisible());
        auto point = [&](double fraction) {
            return plot->mapToScene({plot->width() * fraction, plot->height() * .5}).toPoint();
        };
        const QPoint a = point(reverse ? .71 : .31), z = point(reverse ? .23 : .63);
        const qint64 before = b.viewStart(), span = b.viewEnd() - before;
        auto stamp = [&](QPoint p) {
            return before + qRound64(plot->mapFromScene(p).x() / plot->width() * span);
        };
        const auto min = std::min(stamp(a), stamp(z)), max = std::max(stamp(a), stamp(z));
        QTest::mousePress(ui.win, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(ui.win, z, 20);
        QVERIFY(ui.win->property("selectingRange").toBool());
        QTest::mouseRelease(ui.win, Qt::LeftButton, Qt::NoModifier, z);
        QVERIFY(waitFor([&] { return fresh(b) && b.viewEnd() - b.viewStart() < span; }));
        QVERIFY(std::abs(b.viewStart() - min) <= 1 && std::abs(b.viewEnd() - max) <= 1);
        QCOMPARE(b.focusStart(), b.viewStart());
        QCOMPARE(b.focusEnd(), b.viewEnd());
        QVERIFY(!b.live());
        QCOMPARE(b.sessionId(), id);
    }
    auto *plot = findItem(ui.win->contentItem(), "timelineInput12");
    QVERIFY(plot);
    const auto left = plot->mapToScene({70, 30}).toPoint(), right = plot->mapToScene({150, 30}).toPoint();
    auto from = b.viewStart(), length = b.viewEnd() - from;
    QTest::mousePress(ui.win, Qt::LeftButton, Qt::ShiftModifier, left);
    QTest::mouseMove(ui.win, right, 20);
    QTest::mouseRelease(ui.win, Qt::LeftButton, Qt::ShiftModifier, right);
    QVERIFY(waitFor([&] { return fresh(b) && b.viewStart() < from; }));
    QCOMPARE(b.viewEnd() - b.viewStart(), length);
    b.setViewSeconds(300);
    QCOMPARE(b.viewEnd() - b.viewStart(), initial);
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}

void Integration::latencyColors() {
    const QColor background("#121c25");
    QQuickWindow window;
    window.setColor(background);
    window.resize(320, 240);
    GraphView timeline(window.contentItem());
    timeline.setSize(QSizeF(320, 240));
    timeline.setCeiling(300);
    window.show();
    QVERIFY(waitFor([&] { return window.isExposed(); }));
    const auto image = window.grabWindow();
    QVERIFY(!image.isNull());
    const auto pixelAt = [&](const QImage &frame, double fraction) {
        const auto y = timeline.height() - 2 - fraction * (timeline.height() - 5);
        return frame.pixelColor(qRound(55 * frame.devicePixelRatio()), qRound(y * frame.devicePixelRatio()));
    };
    const auto normal = pixelAt(image, 95.0 / 300);
    const auto amber = pixelAt(image, 105.0 / 300);
    const auto red = pixelAt(image, 205.0 / 300);
    QCOMPARE(normal, background);
    QCOMPARE(pixelAt(image, 195.0 / 300), amber);
    QVERIFY(amber.red() > normal.red() + 8 && amber.green() > normal.green() + 5);
    QVERIFY(red.red() > amber.red() && red.green() < amber.green());

    // A fast route can fill the vertical axis without reaching the warning threshold.
    timeline.setCeiling(80);
    const auto fast = window.grabWindow();
    QVERIFY(!fast.isNull());
    QCOMPARE(pixelAt(fast, 0.9), normal);
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
    b.selectSession(ref);
    b.closeSession(v6);
    QVERIFY(waitFor([&] { return b.closedSessions().size() == 1; }));
    QCOMPARE(b.sessionId(), ref);
    b.reopenSession(v6);
    QVERIFY(waitFor([&] { return b.sessionId() == v6 && !b.running() && fresh(b); }));
    QTest::qWait(800);
    b.goLive();
    QVERIFY(waitFor([&] { return fresh(b); }));
    const auto stoppedEnd = b.viewEnd();
    QTest::qWait(800);
    b.goLive();
    QCOMPARE(b.viewEnd(), stoppedEnd);
    QVERIFY2(ui.warnings.isEmpty(), qPrintable(ui.warnings.join('\n')));
}
} // namespace

int main(int argc, char **argv) {
    QTemporaryDir config;
    if (!config.isValid())
        return 1;
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<GraphView>("Hopline.Graphics", 1, 0, "GraphView");
    Integration suite;
    return QTest::qExec(&suite, argc, argv);
}
#include "integration_test.moc"
