#include "backend.h"
#include "graph.h"
#include "history.h"
#include "probe.h"
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGuiApplication>
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

int report(QCoreApplication &app, const QString &host, int family, int interval, int seconds) {
    QTemporaryDir dir;
    History history(dir.filePath("report.sqlite"));
    if (!dir.isValid() || !history.open())
        return fail(history.error());
    const auto session = history.createSession(host, family, interval);
    if (session <= 0)
        return fail(history.error());
    Probe probe;
    QString status;
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
            {"target", host}, {"family", family}, {"status", status}, {"rows", summary.value("rows")}};
        QTextStream(stdout) << QJsonDocument::fromVariant(result).toJson(QJsonDocument::Indented);
        app.exit(fatalError ? 1 : 0);
    };
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
    QTimer::singleShot(0, &app, [&] { probe.start(session, host, family, interval); });
    QObject::connect(&probe, &Probe::finished, &app, [&](qint64) { QTimer::singleShot(0, &app, finish); });
    QTimer::singleShot(seconds * 1000, &app, [&] { probe.finish(session); });
    return app.exec();
}

// A disposable trace lets users explore historical focus offline.
bool seedExample(const QString &file) {
    History h(file);
    if (!h.open())
        return false;
    const auto id = h.createSession("edge.example.net", 6, 1000);
    if (id <= 0)
        return false;
    const auto end = QDateTime::currentMSecsSinceEpoch();
    QVector<Sample> batch;
    for (int round = 0; round < 1800; ++round) {
        const auto stamp = end - (1799 - round) * 1000LL;
        for (int hop = 1; hop <= 9; ++hop) {
            const bool incident = round >= 1660 && round < 1682 && hop >= 5;
            const auto rtt = 0.55 + 1.7 * hop + 0.13 * hop * hop +
                             1.8 * std::abs(std::sin(round * 0.14 + hop)) +
                             (incident ? 180 + 35 * std::sin(round * 0.4) : 0);
            const auto lost = (incident && round % 5 == 0) || (hop == 3 && round % 11 == 0);
            batch.push_back({id, stamp, hop, lost ? QString() : QString("2001:db8:%1::1").arg(hop),
                             lost ? -1.0 : rtt, hop == 9 && !lost});
        }
        if (batch.size() >= 900) {
            if (!h.append(batch))
                return false;
            batch.clear();
        }
    }
    return true;
}

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
} // namespace

int main(int argc, char **argv) {
    bool graphical = true;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--report" || arg.startsWith("--report=") || arg == "--help" || arg == "--help-all" ||
            arg == "-h" || arg == "-v" || arg == "--version")
            graphical = false;
    }
    std::unique_ptr<QCoreApplication> app;
    if (graphical)
        app = std::make_unique<QGuiApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    app->setApplicationName("hopline");
    app->setApplicationVersion("0.2.0");
    QCommandLineParser parser;
    parser.setApplicationDescription("Hopline — continuous IPv4 and IPv6 path monitoring");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("target", "Start a new trace when the window opens.", "[target]");
    parser.addOptions(
        {{"report", "Collect a finite trace and print per-hop JSON without opening a window.", "target"},
         {"seconds", "Duration in seconds for a headless report (1–3600).", "seconds", "10"},
         {"family", "Select auto, 4, or 6.", "family", "auto"},
         {"interval", "Time between probe rounds in milliseconds (250–60000).", "milliseconds", "1000"},
         {"database", "Open a specific SQLite history file.", "file"},
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
    const int family = familyText.toInt();
    const auto args = parser.positionalArguments();
    if (args.size() > 1)
        return fail("Supply at most one target.");
    if (parser.isSet("report")) {
        if (!args.isEmpty() || parser.isSet("demo") || parser.isSet("database") ||
            parser.value("report").trimmed().isEmpty())
            return fail("Use --report with a single target and a temporary recording.");
        return report(*app, parser.value("report"), family, interval, seconds);
    }
    if (parser.isSet("demo") && (!args.isEmpty() || parser.isSet("database")))
        return fail("Use --demo alone to open a temporary example.");
    QGuiApplication::setDesktopFileName("hopline");
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<GraphView>("Hopline.Graphics", 1, 0, "GraphView");
    QString file = parser.value("database");
    std::unique_ptr<QTemporaryDir> demo;
    if (parser.isSet("demo")) {
        demo = std::make_unique<QTemporaryDir>();
        file = demo->filePath("example.sqlite");
        if (!demo->isValid() || !seedExample(file))
            return fail("Could not create the sample recording.");
    } else if (file.isEmpty()) {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        file = QDir(path).filePath("history.sqlite");
    }
    Backend backend(file);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
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
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return fail("Could not load the Hopline window.");
    if (!args.isEmpty())
        QTimer::singleShot(0, &backend, [&] { backend.startTrace(args.first(), family, interval); });
    return app->exec();
}
