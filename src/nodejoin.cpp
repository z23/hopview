#include "nodejoin.h"
#include "nodeconfig.h"
#include "receiverendpoint.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUuid>
#ifdef Q_OS_LINUX
#include <csignal>
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace telemetry {
#ifdef Q_OS_LINUX
namespace {
// The terminal settings to put back if a signal ends the process at the hidden prompt.
termios promptSettings;
volatile std::sig_atomic_t promptHidden = 0;
void restorePrompt(int signal) {
    if (promptHidden)
        ::tcsetattr(STDIN_FILENO, TCSANOW, &promptSettings);
    std::signal(signal, SIG_DFL);
    ::raise(signal);
}

QString readKey() {
    QByteArray input;
    if (::isatty(STDIN_FILENO)) {
        QProcess prompt;
        prompt.setInputChannelMode(QProcess::ForwardedInputChannel);
        prompt.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        prompt.start("/usr/bin/systemd-ask-password", {"--timeout=0", "Hopview enrollment key:"});
        if (prompt.waitForStarted()) {
            if (!prompt.waitForFinished(-1) || prompt.exitCode() != 0)
                throw Error("Enrollment key entry cancelled");
            input = prompt.readAllStandardOutput().trimmed();
        } else if (prompt.error() == QProcess::FailedToStart) {
            termios saved{};
            if (::tcgetattr(STDIN_FILENO, &saved) != 0)
                throw Error("Cannot configure terminal for hidden enrollment key entry");
            // Ctrl-C, a hangup or a kill at the prompt would otherwise leave the shell without echo.
            static constexpr int interrupts[] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP};
            struct sigaction previous[std::size(interrupts)], handler{};
            handler.sa_handler = restorePrompt;
            sigemptyset(&handler.sa_mask);
            promptSettings = saved;
            for (size_t i = 0; i < std::size(interrupts); ++i)
                ::sigaction(interrupts[i], &handler, &previous[i]);
            const auto restore = qScopeGuard([&] {
                if (promptHidden) {
                    ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
                    promptHidden = 0;
                    const char newline = '\n';
                    if (::write(STDERR_FILENO, &newline, 1) < 0) {}
                }
                for (size_t i = 0; i < std::size(interrupts); ++i)
                    ::sigaction(interrupts[i], &previous[i], nullptr);
            });
            termios hidden = saved;
            hidden.c_lflag &= ~tcflag_t(ECHO);
            promptHidden = 1;
            if (::tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
                promptHidden = 0;
                throw Error("Cannot disable terminal echo for enrollment key entry");
            }
            const char label[] = "Hopview enrollment key: ";
            if (::write(STDERR_FILENO, label, sizeof(label) - 1) < 0) {}
            QFile tty;
            if (!tty.open(STDIN_FILENO, QIODevice::ReadOnly))
                throw Error("Cannot read enrollment key from terminal");
            const auto line = tty.readLine(160);
            if (line.isEmpty())
                throw Error("Enrollment key entry cancelled");
            input = line.trimmed();
        } else {
            throw Error("Enrollment key entry cancelled");
        }
    } else {
        QFile pipe;
        if (!pipe.open(STDIN_FILENO, QIODevice::ReadOnly))
            throw Error("Cannot read enrollment key from standard input");
        input = pipe.readLine(160).trimmed();
    }
    return QString::fromLatin1(input);
}
}
#endif

EnrollmentKey parseEnrollmentKey(const QString &text) {
    static const QRegularExpression format(QStringLiteral(R"(\Ahv1\.([a-f0-9]{64})(?:\.([a-f0-9]{64}))?\z)"));
    const auto match = format.match(text);
    if (!match.hasMatch())
        throw Error("Invalid enrollment key. Copy the whole key from the desktop.");
    return {match.captured(1), QByteArray::fromHex(match.captured(2).toLatin1())};
}

int joinCollector(const QString &endpoint) {
#ifdef Q_OS_LINUX
    ReceiverEndpoint address;
    if (!parseReceiverEndpoint(endpoint, address))
        throw Error("Expected a receiver hostname, address, or [IPv6]:port");
    if (geteuid() != 0)
        throw Error("Run hopview-node join as root to configure the collector service");
    const auto *group = ::getgrnam("hopview");
    if (!group)
        throw Error("Install the node package to create the hopview service account");
    const auto gid = group->gr_gid;
    const QString path = "/etc/hopview/collector.json";
    const auto dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir) || ::chown(QFile::encodeName(dir).constData(), 0, gid) != 0 ||
        ::chmod(QFile::encodeName(dir).constData(), 0750) != 0)
        throw Error("Cannot prepare the collector configuration directory");
    QLockFile lock(path + ".join.lock");
    if (!lock.tryLock())
        throw Error("Another enrollment is in progress");
    QJsonObject existing;
    const bool present = QFileInfo::exists(path);
    if (present) {
        const auto doc = QJsonDocument::fromJson(readFile(path, 1 << 20));
        if (!doc.isObject())
            throw Error("Existing collector configuration is invalid. Back it up and remove it to enroll.");
        existing = doc.object();
        const auto example = QJsonDocument::fromJson(readFile(":/defaults/collector.json", 4096)).object();
        if (!existing.value("managed").toBool() && existing != example)
            throw Error("This collector is locally configured. Back up and remove its configuration to enroll.");
    }
    const auto key = readKey();
    parseEnrollmentKey(key);
    const auto &host = address.host;
    const int port = address.port;
    if (existing.value("managed").toBool()) {
        if (existing.value("receiver") != host || existing.value("port").toInt() != port ||
            existing.value("enrollmentKey") != key)
            throw Error("This VM is already enrolled using different credentials. Its configuration was preserved.");
    } else {
        const auto nodeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject settings{{"managed", true}, {"receiver", host}, {"port", port}, {"enrollmentKey", key},
                             {"nodeId", nodeId}, {"spool", "/var/lib/hopview-collector/" + nodeId + ".sqlite"}};
        auto temporary = QFile::encodeName(path + ".XXXXXX");
        const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
        if (fd < 0)
            throw Error("Cannot create the protected collector configuration");
        const auto cleanup = qScopeGuard([&] {
            ::close(fd);
            ::unlink(temporary.constData());
        });
        const auto data = QJsonDocument(settings).toJson();
        QFile file;
        if (!file.open(fd, QIODevice::WriteOnly) || ::fchown(fd, 0, gid) != 0 ||
            ::fchmod(fd, 0640) != 0 || file.write(data) != data.size() || !file.flush() ||
            ::fsync(fd) != 0 || ::rename(temporary.constData(), QFile::encodeName(path).constData()) != 0)
            throw Error("Could not save the protected collector configuration");
    }
    if (QProcess::execute("/usr/bin/systemctl", {"enable", "--now", "hopview-collector.service"}) != 0)
        throw Error("Enrollment saved. Could not start the collector service; rerun join to retry.");
    log("Collector service enabled. Check its connection under Collectors in Hopview.");
    return 0;
#else
    Q_UNUSED(endpoint)
    throw Error("Service enrollment requires a Linux collector. Start this node with collect COLLECTOR.json.");
#endif
}
}
