#include "diagnostics.h"
#include "nodeconfig.h"
#include "nodejoin.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#ifdef Q_OS_LINUX
#include <QSocketNotifier>
#include <csignal>
#include <sys/signalfd.h>
#else
#include "portableexit.h"
#endif
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <QTimer>
#endif

int main(int argc, char **argv) {
    CurlGlobalInit curlInit;
#ifdef Q_OS_UNIX
    umask(0077);
#endif
    const bool joining = argc > 1 && QByteArray(argv[1]) == "join";
#ifdef Q_OS_LINUX
    sigset_t signalsToHandle;
    sigemptyset(&signalsToHandle);
    sigaddset(&signalsToHandle, SIGINT);
    sigaddset(&signalsToHandle, SIGTERM);
    if (!joining && sigprocmask(SIG_BLOCK, &signalsToHandle, nullptr) != 0)
        return 1;
#endif
    QCoreApplication app(argc, argv);
    app.setApplicationName("hopview-node");
    const auto args = app.arguments();
    if (args.size() != 3 || (!joining && args[1] != "collect" && args[1] != "receive")) {
        telemetry::log("Usage: hopview-node join RECEIVER[:PORT] | collect COLLECTOR.json | receive RECEIVER.json");
        return args.size() == 2 && (args[1] == "--help" || args[1] == "-h") ? 0 : 2;
    }
    try {
        if (joining)
            return telemetry::joinCollector(args[2]);
#ifdef Q_OS_LINUX
        const int fd = signalfd(-1, &signalsToHandle, SFD_CLOEXEC | SFD_NONBLOCK);
        if (fd < 0)
            throw telemetry::Error("Cannot listen for shutdown signals");
        QSocketNotifier quitSignal(fd, QSocketNotifier::Read);
        QObject::connect(&quitSignal, &QSocketNotifier::activated, &app, [&] {
            quitSignal.setEnabled(false);
            app.quit();
        });
#else
        installPortableExitHandlers(app);
#endif
#ifdef Q_OS_WIN
        if (qEnvironmentVariable("HOPVIEW_PARENT_PIPE") == "1") {
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            if (GetFileType(input) != FILE_TYPE_PIPE)
                throw telemetry::Error("This node needs a parent control pipe");
            auto *watch = new QTimer(&app);
            QObject::connect(watch, &QTimer::timeout, &app, [input] {
                if (!PeekNamedPipe(input, nullptr, 0, nullptr, nullptr, nullptr))
                    QCoreApplication::quit();
            });
            watch->start(250);
        }
#endif
        auto doc = QJsonDocument::fromJson(telemetry::readFile(args[2], 1024 * 1024));
        if (!doc.isObject())
            throw telemetry::Error("Configuration must be a JSON object");
        const auto base = QFileInfo(args[2]).absolutePath();
        return args[1] == "collect" ? telemetry::collect(doc.object(), base)
                                    : telemetry::receive(doc.object(), base);
    } catch (const std::exception &e) {
        telemetry::log(QString::fromUtf8(e.what()));
        return 1;
    }
}
