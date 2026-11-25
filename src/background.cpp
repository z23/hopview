#include "background.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#else
#include "portableexit.h"
#include <QIcon>
#include <QSystemTrayIcon>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <cerrno>
#include <csignal>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t signalWrite = -1;
void requestExit(int) {
    const int saved = errno;
    const char byte = 1;
    if (signalWrite >= 0) (void)::write(signalWrite, &byte, 1);
    errno = saved;
}
}
#elif defined(Q_OS_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#endif

void BackgroundInstance::enableGracefulExit() {
#ifdef Q_OS_LINUX
    int ends[2];
    if (pipe2(ends, O_NONBLOCK | O_CLOEXEC) != 0)
        return;
    signalWrite = ends[1];
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_handler = requestExit;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    auto *notifier = new QSocketNotifier(ends[0], QSocketNotifier::Read, qApp);
    connect(notifier, &QSocketNotifier::activated, qApp, [notifier] {
        notifier->setEnabled(false);
        QCoreApplication::quit();
    });
    connect(notifier, &QObject::destroyed, [read = ends[0], write = ends[1]] {
        signalWrite = -1;
        ::close(read);
        ::close(write);
    });
#else
    installPortableExitHandlers(*qApp);
#endif
}

BackgroundInstance::Result BackgroundInstance::acquire(const QString &database, const QJsonObject &request,
                                                         QString &error) {
#ifdef Q_OS_MACOS
    // Keep the Unix socket within sockaddr_un's path length even for long home names.
    const QString runtime = QStringLiteral("/tmp/hopview-%1").arg(getuid());
    const auto path = QFile::encodeName(runtime);
    struct stat info{};
    if ((mkdir(path.constData(), 0700) != 0 && errno != EEXIST) ||
        lstat(path.constData(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 077) != 0) {
        error = tr("Cannot prepare a private recorder directory.");
        return Failed;
    }
#elif defined(Q_OS_WIN)
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(runtime)) {
        error = tr("Cannot create the recorder directory.");
        return Failed;
    }
#else
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
    if (runtime.isEmpty()) {
        error = tr("A private runtime directory is required for the recorder.");
        return Failed;
    }
    QFileInfo file(database);
    if (request["action"] != "quit" && !QDir().mkpath(file.absolutePath())) {
        error = tr("Cannot create the recording directory.");
        return Failed;
    }
    const QString canonical = file.exists() ? file.canonicalFilePath() :
        QDir(file.absoluteDir().canonicalPath()).absoluteFilePath(file.fileName());
    auto identity = canonical;
#ifdef Q_OS_WIN
    identity = runtime.toCaseFolded() + "/" + canonical.toCaseFolded();
#endif
    const auto hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    // All recorder versions must use the same lock and socket for a given history.
    const QString lockPath = QDir(runtime).filePath("hopline-" + hash);
    lock_ = std::make_unique<QLockFile>(lockPath + ".lock");
#ifdef Q_OS_WIN
    const QString name = "hopview-" + hash;
#else
    const QString name = lockPath;
#endif
    lock_->setStaleLockTime(0);
    if (lock_->tryLock()) {
        if (request["action"] == "quit") {
            error = tr("No recorder is running for this database.");
            return Failed;
        }
        QLocalServer::removeServer(name);
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if (!server_.listen(name)) {
            error = server_.errorString();
            return Failed;
        }
        return Owner;
    }
    if (lock_->error() != QLockFile::LockFailedError) {
        error = tr("Cannot lock the recorder in %1.").arg(runtime);
        return Failed;
    }
    qint64 originalOwner = 0;
    lock_->getLockInfo(&originalOwner, nullptr, nullptr);
    QLocalSocket socket;
    QElapsedTimer startup;
    startup.start();
    do {
        socket.abort();
        socket.connectToServer(name);
        if (socket.waitForConnected(250))
            break;
        QThread::msleep(25);
    } while (startup.elapsed() < 3000);
    if (socket.state() != QLocalSocket::ConnectedState) {
        error = tr("The existing recorder is unavailable. %1").arg(socket.errorString());
        return Failed;
    }
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    socket.flush();
    if ((!socket.canReadLine() && !socket.waitForReadyRead(15000)) || socket.readLine() != "ok\n") {
        error = tr("The existing recorder did not acknowledge the request.");
        return Failed;
    }
    if (request["action"] == "quit" && originalOwner) {
        // The recorder releases ownership after the probe and database writer have shut down.
        // A sequential quit/relaunch must never send the launch to a retiring process.
        QElapsedTimer shutdown;
        shutdown.start();
        qint64 owner = originalOwner;
        while (lock_->getLockInfo(&owner, nullptr, nullptr) && owner == originalOwner) {
            if (shutdown.elapsed() >= 15000) {
                error = tr("The recorder is still shutting down.");
                return Failed;
            }
            QThread::msleep(25);
        }
    }
    return Forwarded;
}

void BackgroundInstance::handleRequests(std::function<void(const QJsonObject &)> fn) {
    handler_ = std::move(fn);
    connect(&server_, &QLocalServer::newConnection, this, &BackgroundInstance::acceptClients);
    acceptClients();
#ifdef Q_OS_LINUX
    auto bus = QDBusConnection::sessionBus();
    bus.connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", "ActionInvoked", this, SLOT(notificationAction(uint,QString)));
    bus.connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications", "NotificationClosed", this, SLOT(notificationClosed(uint,uint)));
#else
    tray_ = new QSystemTrayIcon(QIcon(":/hopview.svg"), this);
    tray_->setToolTip(tr("Hopview · Open recordings"));
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            handler_({{"action", "show"}});
    });
    // Native tray callbacks do not identify which historical notification was clicked.
    connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] { handler_({{"action", "show"}}); });
    tray_->show();
#endif
}

void BackgroundInstance::acceptClients() {
    while (auto *client = server_.nextPendingConnection()) {
        client->setReadBufferSize(4097);
        connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
        QTimer::singleShot(5000, client, &QLocalSocket::disconnectFromServer);
        auto receive = [this, client] {
            if (client->bytesAvailable() > 4096) {
                client->abort();
                return;
            }
            if (!client->canReadLine())
                return;
            const auto request = QJsonDocument::fromJson(client->readLine()).object();
            const auto action = request["action"].toString();
            if (action == "show" || action == "background" || action == "quit") {
                handler_(request);
                client->write("ok\n");
            }
            client->disconnectFromServer();
        };
        connect(client, &QLocalSocket::readyRead, client, receive);
        receive();
    }
}

void BackgroundInstance::notify(const QVariantMap &event) {
    const double latency = event["latency"].toDouble();
    const auto label = event.value("sourceLabel", event["target"]).toString();
    const auto body = tr("Destination loss %1% · Mean RTT %2")
        .arg(QString::number(event["loss"].toDouble(), 'f', 1),
             latency < 0 ? tr("unavailable") : QString::number(latency, 'f', 1) + " ms");
#ifdef Q_OS_LINUX
    const auto summary = QString("%1 — %2").arg(label.toHtmlEscaped(), event["label"].toString());
    auto message = QDBusMessage::createMethodCall("org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify");
    message.setArguments({QStringLiteral("Hopview"), 0u, QStringLiteral("hopview"), summary, body,
                          QStringList{"default", tr("View interval")}, QVariantMap{}, -1});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, event] {
        QDBusPendingReply<uint> reply = *watcher;
        if (!reply.isError())
            notices_.insert(reply.value(), event);
        watcher->deleteLater();
    });
#else
    if (tray_)
        tray_->showMessage(QString("%1 — %2").arg(label, event["label"].toString()), body);
#endif
}

#ifdef Q_OS_LINUX
void BackgroundInstance::notificationAction(uint id, const QString &action) {
    if (action == "default" && notices_.contains(id))
        emit openEvent(notices_[id]);
}
void BackgroundInstance::notificationClosed(uint id, uint) { notices_.remove(id); }
#endif
