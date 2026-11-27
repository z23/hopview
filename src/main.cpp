#include "probeoptions.h"
#include "backend.h"
#include "background.h"
#include "diagnostics.h"
#include "graph.h"
#include "localreceiver.h"
#include "history.h"
#include "incident.h"
#include "probe.h"
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include <QApplication>
#endif
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <memory>

namespace {
int fail(const QString &message) {
    QTextStream(stderr) << message << Qt::endl;
    return 1;
}

int report(QCoreApplication &app, const QString &host, int family, int interval, int seconds, const ProbeOptions &options) {
    QTemporaryDir dir;
    History history(dir.filePath("report.sqlite"));
    if (!dir.isValid() || !history.open())
        return fail(history.error());
    const auto session = history.createSession(host, family, interval, options.protocol, options.port, options.interfaceName,options.payloadBytes,options.dscp,options.dontFragment);
    if (session <= 0)
        return fail(history.error());
    Probe probe;
    QString status;
    QVariantMap resolved;
    bool fatalError = false;
    auto finish = [&] {
        probe.shutdown();
        const auto end = QDateTime::currentMSecsSinceEpoch();
        auto summary =
            history.view(session, end - (seconds + 5) * 1000, end, end - (seconds + 5) * 1000, end, {}, 32);
        if (summary.value("rows").toList().isEmpty() && !fatalError) {
            fatalError = true;
            status = "No completed probes. " + status;
        }
        QVariantMap result{
            {"target", host}, {"family", family}, {"protocol", options.protocol}, {"port", options.destinationPort()},
            {"interfaceName", options.interfaceName}, {"status", status}, {"rows", summary.value("rows")}, {"endpoint",summary.value("endpoint")}, {"measurement",summary.value("measurement")}, {"endpointFailure",summary.value("endpointFailure")},
            {"payloadBytes",options.payloadBytes},{"dscp",options.dscp},{"dontFragment",options.dontFragment}};
        result.insert(resolved);
        QTextStream(stdout) << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Indented);
        app.exit(fatalError ? 1 : 0);
    };
    QObject::connect(&probe, &Probe::measurement, &app, [&](qint64 id,qint64 stamp,const QVariantMap &m) {
        history.setMeasurement(id,stamp,m);
    });
    QObject::connect(&probe, &Probe::resolved, &app, [&](qint64, QString address, QString method, int port) {
        resolved = {{"resolvedAddress", address}, {"effectiveProtocol", method}, {"effectivePort", port}};
    });
    QObject::connect(&probe, &Probe::status, &app, [&](qint64, QString text, bool fatal) {
        status = text;
        if (fatal) {
            fatalError = true;
            QTimer::singleShot(0, &app, finish);
        }
    });
    QObject::connect(&probe, &Probe::samples, &app, [&](const QVector<Sample> &batch) {
        if (!history.append(batch)) {
            status = history.error();
            fatalError = true;
            QTimer::singleShot(0, &app, finish);
        }
    });
    QTimer::singleShot(0, &app, [&] { probe.start(session, host, family, interval, options.protocol, options.port, options.interfaceName,options.payloadBytes,options.dscp,options.dontFragment); });
    QObject::connect(&probe, &Probe::finished, &app, [&](qint64) { QTimer::singleShot(0, &app, finish); });
    QTimer::singleShot(seconds * 1000, &app, [&] { probe.finish(session); });
    return app.exec();
}

// A disposable trace lets users explore historical focus offline.
bool seedExample(const QString &file) {
    History h(file);
    if (!h.open())
        return false;
    const auto gateway = h.createSession("192.0.2.1", 4, 1000);
    const auto id = h.createSession("edge.example.net", 6, 1000);
    if (id <= 0)
        return false;
    const auto end = QDateTime::currentMSecsSinceEpoch();
    QVector<Sample> batch;
    for (int round = 0; round < 1800; ++round) {
        const auto stamp = end - (1799 - round) * 1000LL;
        batch.push_back({gateway, stamp, 1, "192.0.2.1", 1.3 + 0.4 * std::sin(round * 0.14), true});
        for (int hop = 1; hop <= 9; ++hop) {
            const bool incident = round >= 1660 && round < 1682 && hop >= 5;
            const auto rtt = 0.55 + 1.7 * hop + 0.13 * hop * hop +
                             1.8 * std::abs(std::sin(round * 0.14 + hop)) +
                             (incident ? 180 + 35 * std::sin(round * 0.4) : 0);
            const auto lost = (incident && round % 5 == 0) || (hop == 3 && round % 11 == 0);
            const auto address = hop == 5 && round < 1660 ? QString("2001:db8:5::2") : QString("2001:db8:%1::1").arg(hop);
            batch.push_back({id, stamp, hop, lost ? QString() : address,
                             lost ? -1.0 : rtt, hop == 9 && !lost});
        }
        if (round % 5 == 0) {
            const bool slow = round >= 1660 && round < 1682;
            const double connect = slow ? 195. : 23.;
            const double wait = slow ? 220. : 32.;
            const double elapsed = connect * 2 + 2 + wait;
            const bool failed = round == 1670;
            const auto event = failed ? QString("HTTP 503") : QString{};
            if (!h.appendDiagnostic(id, {{"kind", "service"},
                                         {"t", stamp},
                                         {"url", "https://edge.example.net/"},
                                         {"address", "2001:db8:9::1"},
                                         {"family", 6},
                                         {"periodMs", 5000},
                                         {"status", failed ? 503 : 200},
                                         {"event", event},
                                         {"error", event},
                                         {"dns", 2.},
                                         {"connect", connect},
                                         {"tls", connect},
                                         {"firstByte", wait},
                                         {"ttfb", elapsed},
                                         {"total", elapsed}}))
                return false;
            if (!h.appendDiagnostic(id, {{"kind", "interface"},
                                         {"t", stamp},
                                         {"name", "eth0"},
                                         {"periodMs", 5000},
                                         {"carrier", 1},
                                         {"ap", ""},
                                         {"rx", 85000. + 10000. * std::sin(round * 0.1)},
                                         {"tx", 43000.},
                                         {"rxDrops", 0},
                                         {"txDrops", 0},
                                         {"rxErrors", 0},
                                         {"txErrors", 0},
                                         {"event", ""}}))
                return false;
        }
        if (batch.size() >= 900) {
            if (!h.append(batch))
                return false;
            batch.clear();
        }
    }
    return h.append(batch) && h.addNote(id,end - 139000, "Upstream latency increased");
}

#ifdef Q_OS_LINUX
QColor desktopAccent(const QString &fileName) {
    QFile file(fileName);
    if (file.open(QIODevice::ReadOnly)) {
        static const QRegularExpression rx(
            QStringLiteral(R"re((?:^|\n)\s*accent\s*=\s*["'](#[a-fA-F0-9]{6})["'])re"));
        const auto match = rx.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch())
            return QColor(match.captured(1));
    }
    return QColor("#58d5af");
}
#endif
} // namespace

int main(int argc, char **argv) {
    CurlGlobalInit curlInit;
    bool graphical = true;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--report" || arg.startsWith("--report=") || arg == "--help" || arg == "--help-all" || arg == "--quit" ||
            arg == "-h" || arg == "-v" || arg == "--version")
            graphical = false;
    }
    std::unique_ptr<QCoreApplication> app;
    if (graphical)
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        app = std::make_unique<QApplication>(argc, argv);
#else
        app = std::make_unique<QGuiApplication>(argc, argv);
#endif
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    app->setApplicationName("hopview");
    app->setApplicationVersion("0.6.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("Hopview — continuous IPv4 and IPv6 path monitoring");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("target", "Start a new trace when the window opens.", "[target]");
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const QString methods = QStringLiteral("Use auto or icmp for native IPv4/IPv6 echo probes.");
#else
    const QString methods = QStringLiteral("Select auto, icmp, udp, or tcp.");
#endif
    parser.addOptions(
        {{"report", "Collect a finite trace and print per-hop JSON without opening a window.", "target"},
         {"seconds", "Duration in seconds for a headless report (1–3600).", "seconds", "10"},
         {"family", "Select auto, 4, or 6.", "family", "auto"},
         {"protocol", methods, "method", "auto"},
         {"port", "Fixed destination port for TCP (default 443) or UDP (default 33434).", "port"},
         {"payload-bytes", "ICMP/UDP payload in bytes, excluding network and transport headers.", "bytes", "16"},
         {"dscp", "Set the IP differentiated services code point (0–63).", "value", "0"},
         {"dont-fragment", "Require unfragmented datagrams and record path MTU errors."},
         {"interface", "Bind probes to this outgoing network interface.", "name"},
         {"interval", "Time between probe rounds in milliseconds (250–60000).", "milliseconds", "1000"},
         {"background", "Keep recording after window close and start with the window closed."},
         {"quit", "Shut down the recorder for this database and flush pending samples."},
         {"database", "Open a specific history file.", "file"},
         {"replay", "Open an exported incident in an offline window.", "file"},
         {"demo", "Explore an isolated example session without sending probes."}});
    parser.process(*app);
    bool okInterval = false, okDuration = false;
    const auto interval = parser.value("interval").toInt(&okInterval);
    const auto seconds = parser.value("seconds").toInt(&okDuration);
    const auto familyText = parser.value("family");
    if (familyText != "auto" && familyText != "4" && familyText != "6")
        return fail("Family must be auto, 4, or 6.");
    if (!okInterval || interval < 250 || interval > 60000 || !okDuration || seconds < 1 || seconds > 3600)
        return fail("Interval must be 250–60000 ms and duration must be 1–3600 seconds.");
    bool okPort = true;
    const int port = parser.isSet("port") ? parser.value("port").toInt(&okPort) : 0;
    bool okSize = false, okDscp = false;
    const ProbeOptions options{parser.value("protocol").toLower(), port, parser.value("interface"),
        parser.value("payload-bytes").toInt(&okSize),parser.value("dscp").toInt(&okDscp),parser.isSet("dont-fragment")};
    if (!okSize || !okDscp || !okPort || (parser.isSet("port") && port < 1) || !options.valid())
        return fail("Check the protocol, port, payload (8–65507 bytes) and DSCP (0–63). TCP uses the system SYN size.");
    const int family = familyText.toInt();
    const auto args = parser.positionalArguments();
    if (!args.isEmpty() || parser.isSet("report")) {
        const auto error = localProbeError(options, family);
        if (!error.isEmpty()) return fail(error);
    }
    if (args.size() > 1)
        return fail("Supply at most one target.");
    const bool isolated = parser.isSet("demo") || parser.isSet("replay");
    if ((parser.isSet("background") || parser.isSet("quit")) && (isolated || parser.isSet("report")))
        return fail("Recorder options require a regular recording database.");
    if (parser.isSet("quit") && (!args.isEmpty() || parser.isSet("background")))
        return fail("Use --quit alone or with --database.");
    if (parser.isSet("report")) {
        if (!args.isEmpty() || parser.isSet("demo") || parser.isSet("database") || parser.isSet("replay") ||
            parser.value("report").trimmed().isEmpty())
            return fail("Use --report with a single target and a temporary recording.");
        return report(*app, parser.value("report"), family, interval, seconds, options);
    }
    if (parser.isSet("demo") && (!args.isEmpty() || parser.isSet("database")))
        return fail("Use --demo alone to open a temporary example.");
    if (parser.isSet("replay") && (!args.isEmpty() || parser.isSet("database") || parser.isSet("demo")))
        return fail("Use --replay with a single incident file.");
    QString file = parser.value("database");
    IncidentRange incident;
    std::unique_ptr<QTemporaryDir> demo;
    if (parser.isSet("demo")) {
        demo = std::make_unique<QTemporaryDir>();
        file = demo->filePath("example.sqlite");
        if (!demo->isValid() || !seedExample(file))
            return fail("Could not create the sample recording.");
    } else if (parser.isSet("replay")) {
        demo = std::make_unique<QTemporaryDir>();
        if (!demo->isValid()) return fail(demo->errorString());
        file = demo->filePath("replay.sqlite");
        History imported(file);
        QString error;
        if (!importIncident(parser.value("replay"), imported, incident, error))
            return fail(error);
    } else if (file.isEmpty()) {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        file = QDir(path).filePath("history.sqlite");
        const auto previous = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                  .filePath("hopline/history.sqlite");
        if (!QFileInfo::exists(file) && QFileInfo(previous).isFile())
            file = previous;
    }
    BackgroundInstance instance;
    const QString action = parser.isSet("quit") ? "quit" : parser.isSet("background") ? "background" : "show";
    const QJsonObject request{{"action", action}, {"target", args.value(0)}, {"family", family},
                              {"interval", interval}, {"protocol", options.protocol},
                              {"port", options.port}, {"interface", options.interfaceName},{"payloadBytes",options.payloadBytes},{"dscp",options.dscp},{"dontFragment",options.dontFragment}};
    if (!isolated) {
        QString error;
        const auto result = instance.acquire(file, request, error);
        if (result == BackgroundInstance::Failed)
            return fail(error);
        if (result == BackgroundInstance::Forwarded)
            return 0;
    }
    QGuiApplication::setDesktopFileName("hopview");
    if (!isolated) {
        const QDir config(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
        const auto previous = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                                  .filePath("hopline/appearance.ini");
        const auto appearance = config.filePath("appearance.ini");
        if (!QFileInfo::exists(appearance) && QFileInfo(previous).isFile() && config.mkpath("."))
            QFile::copy(previous, appearance);
    }
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<LocalReceiver>("Hopview.Graphics", 1, 0, "LocalReceiver");
    qmlRegisterType<GraphView>("Hopview.Graphics", 1, 0, "GraphView");
    Backend backend(file);
    if (isolated)
        backend.setTemporaryHistory();
    if (parser.isSet("replay")) {
        backend.setReplayMode();
        backend.restoreIncident(incident);
    }
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
#ifdef Q_OS_LINUX
    const auto themeDir = QDir::homePath() + "/.local/state/omarchy/current/theme";
    const auto themeFile = themeDir + "/colors.toml";
    QFileSystemWatcher watcher;
    auto applyTheme = [&] {
        for (const auto &path : {QFileInfo(themeDir).absolutePath(), themeDir, themeFile})
            if (QFileInfo::exists(path) && !watcher.files().contains(path) &&
                !watcher.directories().contains(path))
                watcher.addPath(path);
        engine.rootContext()->setContextProperty("themeAccent", desktopAccent(themeFile));
    };
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &engine, applyTheme);
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &engine, applyTheme);
    applyTheme();
#else
    engine.rootContext()->setContextProperty("themeAccent", QColor("#58d5af"));
#endif
    engine.setInitialProperties({{"startHidden", action == "background"}});
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return fail("Could not load the Hopview window.");
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    if (!window)
        return fail("Could not initialize the Hopview window.");
    if (!isolated) {
        backend.enableRecorder();
        BackgroundInstance::enableGracefulExit();
        auto updateQuitPolicy = [&] { QGuiApplication::setQuitOnLastWindowClosed(!backend.backgroundEnabled()); };
        QObject::connect(&backend, &Backend::monitoringChanged, app.get(), updateQuitPolicy);
        updateQuitPolicy();
        QObject::connect(window, &QWindow::visibleChanged, &backend, &Backend::setWindowVisible);
        backend.setWindowVisible(window->isVisible());
        auto activate = [window] {
            window->show();
            window->raise();
            window->requestActivate();
        };
        QObject::connect(&backend, &Backend::destinationAlert, &instance, &BackgroundInstance::notify);
        QObject::connect(&instance, &BackgroundInstance::openEvent, &backend, [&, activate](QVariantMap e) {
            activate();
            backend.showAlert(e["session"].toLongLong(), e["startMs"].toLongLong(), e["endMs"].toLongLong());
        });
        auto handle = [&, activate](const QJsonObject &r) {
            if (r["action"] == "quit") {
                QTimer::singleShot(0, app.get(), &QCoreApplication::quit);
                return;
            }
            if (r["action"] == "background") {
                backend.setBackgroundEnabled(true);
                if (backend.backgroundEnabled())
                    window->close();
                else
                    activate();
            } else {
                activate();
            }
            const auto target = r["target"].toString().trimmed();
            if (target.isEmpty())
                return;
            ProbeOptions probeOptions{r["protocol"].toString(), r["port"].toInt(), r["interface"].toString()};
            backend.startTrace(target, r["family"].toInt(), r["interval"].toInt(),
                               probeOptions.protocol, probeOptions.port, probeOptions.interfaceName, true,r["payloadBytes"].toInt(16),r["dscp"].toInt(),r["dontFragment"].toBool());
        };
        handle(request);
        instance.handleRequests(handle);
    }
    return app->exec();
}
