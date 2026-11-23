#include "backend.h"
#include "graph.h"
#include "history.h"
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
    void probeSettings();
    void routeHistory();
    void latencyColors();
    void tabControlsAndScale();
    void rangeSelection();
    void compareTargets();
    void saveFocus();
    void replayWhileProbing();
};

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
