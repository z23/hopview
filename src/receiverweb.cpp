#include "receiverweb.h"
#include "history.h"
#include "nodeconfig.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSslSocket>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace telemetry {
namespace {
constexpr int maxGlobalConnections = 32;
constexpr int maxPerIpConnections = 8;
constexpr int maxAuthFailures = 10;
constexpr qint64 authWindowMs = 60000;
constexpr int headerDeadlineMs = 15000;
constexpr int transferDeadlineMs = 60000;

struct Response {
    QByteArray status, type, body, extraHeaders;
};

QString peerKey(const QSslSocket *socket) {
    auto addr = socket->peerAddress();
    bool ok = false;
    const quint32 v4 = addr.toIPv4Address(&ok);
    if (ok)
        addr = QHostAddress(v4);
    return addr.toString();
}

Response query(History &history, const QUrl &url) {
    if (!history.openReadOnly())
        return {"503 Service Unavailable", "text/plain", "History unavailable", {}};
    if (url.path() == "/api/sessions") {
        const auto sessions = history.sessions(true);
        if (!history.error().isEmpty())
            return {"503 Service Unavailable", "text/plain", "History unavailable", {}};
        return {"200 OK", "application/json",
                QJsonDocument(QJsonArray::fromVariantList(sessions)).toJson(QJsonDocument::Compact), {}};
    }
    const QUrlQuery params(url);
    bool idOk, fromOk, throughOk;
    const auto id = params.queryItemValue("session").toLongLong(&idOk);
    const auto from = params.queryItemValue("from").toLongLong(&fromOk);
    const auto through = params.queryItemValue("through").toLongLong(&throughOk);
    if (!idOk || !fromOk || !throughOk || id < 1 || from < 0 || from > through || through > maxStamp ||
        through - from > 31LL * 86400000)
        return {"400 Bad Request", "text/plain", "Choose a recording and up to 31 days of UTC history", {}};
    const auto sessions = history.sessions(true, id);
    if (!history.error().isEmpty())
        return {"503 Service Unavailable", "text/plain", "History unavailable", {}};
    if (sessions.isEmpty())
        return {"404 Not Found", "text/plain", "Remote recording unavailable", {}};
    auto view = history.view(id, from, through, from, through, {0}, 720);
    if (!history.error().isEmpty())
        return {"503 Service Unavailable", "text/plain", "History unavailable", {}};
    return {"200 OK", "application/json",
            QJsonDocument(QJsonObject::fromVariantMap(view)).toJson(QJsonDocument::Compact), {}};
}

void respond(QSslSocket *socket, const Response &r) {
    QByteArray headers = "HTTP/1.1 " + r.status + "\r\nContent-Type: " + r.type +
                         "\r\nStrict-Transport-Security: max-age=31536000\r\n"
                         "X-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n"
                         "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; object-src "
                         "'none'; frame-ancestors 'none'\r\n";
    if (r.status.startsWith("401"))
        headers += "WWW-Authenticate: Basic realm=\"Hopview reader\"\r\n";
    headers += r.extraHeaders;
    socket->write(headers + "Content-Length: " + QByteArray::number(r.body.size()) +
                  "\r\nConnection: close\r\n\r\n" + r.body);
    socket->disconnectFromHost();
}

class Viewer final : public QTcpServer {
  public:
    Viewer(QString database, const QByteArray &token, QSslConfiguration ssl)
        : database_(std::move(database)),
          expected_(QCryptographicHash::hash("reader:" + token, QCryptographicHash::Sha256)),
          ssl_(std::move(ssl)) {
        readers_.setMaxThreadCount(3);
        clock_.start();
    }

    ~Viewer() override {
        readers_.waitForDone();
        std::lock_guard lock(poolMutex_);
        pool_.clear();
    }

  private:
    struct AuthWindow {
        qint64 windowStartMs = 0;
        int failures = 0;
    };

    std::unique_ptr<History> acquireReader() {
        std::unique_ptr<History> reader;
        {
            std::lock_guard lock(poolMutex_);
            if (!pool_.empty()) {
                reader = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        if (!reader)
            reader = std::make_unique<History>(database_);
        return reader;
    }

    void releaseReader(std::unique_ptr<History> reader) {
        if (!reader || !reader->error().isEmpty())
            return;
        std::lock_guard lock(poolMutex_);
        if (pool_.size() < 3)
            pool_.push_back(std::move(reader));
    }

    void pruneAuthWindows(qint64 nowMs) {
        for (auto it = authFailures_.begin(); it != authFailures_.end();) {
            if (nowMs - it->windowStartMs >= authWindowMs)
                it = authFailures_.erase(it);
            else
                ++it;
        }
        if (authFailures_.size() >= 4096)
            authFailures_.clear();
    }

    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor) || active_ >= maxGlobalConnections) {
            delete socket;
            return;
        }
        const auto peer = peerKey(socket);
        if (activeByIp_.value(peer) >= maxPerIpConnections) {
            delete socket;
            return;
        }
        ++active_;
        ++activeByIp_[peer];
        connect(socket, &QObject::destroyed, this, [this, peer] {
            --active_;
            auto it = activeByIp_.find(peer);
            if (it != activeByIp_.end() && --(*it) <= 0)
                activeByIp_.erase(it);
        });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        deadline->start(headerDeadlineMs);
        socket->setSslConfiguration(ssl_);
        socket->setReadBufferSize(8193);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, deadline, peer] {
            if (!socket->isEncrypted() || socket->property("handled").toBool())
                return;
            auto bytes = socket->peek(8193);
            if (bytes.size() > 8192) {
                socket->abort();
                return;
            }
            const auto end = bytes.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            socket->setProperty("handled", true);
            deadline->start(transferDeadlineMs);
            const auto lines = bytes.first(end).split('\n');
            const auto request = lines.first().trimmed().split(' ');
            QByteArray credentials;
            bool presented = false;
            for (const auto &line : lines) {
                const auto split = line.indexOf(':');
                if (split > 0 && line.first(split).compare("Authorization", Qt::CaseInsensitive) == 0) {
                    presented = true;
                    const auto value = line.sliced(split + 1).trimmed();
                    if (value.left(6).compare("Basic ", Qt::CaseInsensitive) == 0)
                        credentials =
                            QByteArray::fromBase64(value.sliced(6), QByteArray::AbortOnBase64DecodingErrors);
                }
            }
            const auto digest = QCryptographicHash::hash(credentials, QCryptographicHash::Sha256);
            unsigned difference = 0;
            for (int i = 0; i < digest.size(); ++i)
                difference |= static_cast<unsigned char>(digest[i] ^ expected_[i]);
            if (difference) {
                // Only a wrong password counts toward the limit, and the correct one is never
                // refused. A request without credentials is how every browser starts, and one
                // stale script must not lock out the readers who share its address.
                const auto nowMs = clock_.elapsed();
                auto window = authFailures_.find(peer);
                if (window != authFailures_.end() && nowMs - window->windowStartMs >= authWindowMs) {
                    authFailures_.erase(window);
                    window = authFailures_.end();
                }
                if (presented) {
                    if (window != authFailures_.end() && window->failures >= maxAuthFailures) {
                        const auto retryAfter =
                            std::max<qint64>(1, (authWindowMs - (nowMs - window->windowStartMs) + 999) / 1000);
                        respond(socket, {"429 Too Many Requests", "text/plain", "Too many authentication failures",
                                         "Retry-After: " + QByteArray::number(retryAfter) + "\r\n"});
                        return;
                    }
                    if (window == authFailures_.end()) {
                        if (authFailures_.size() >= 4096)
                            pruneAuthWindows(nowMs);
                        window = authFailures_.insert(peer, {nowMs, 0});
                    }
                    if (++window->failures == maxAuthFailures)
                        log(QString("Rate-limiting viewer authentication failures from %1").arg(peer));
                }
                respond(socket, {"401 Unauthorized", "text/plain", "Viewer credentials required", {}});
                return;
            }
            authFailures_.remove(peer);
            if (request.size() != 3 || request.first() != "GET") {
                respond(socket, {"405 Method Not Allowed", "text/plain", "Use GET", {}});
                return;
            }
            const auto url = QUrl::fromEncoded(request.at(1), QUrl::StrictMode);
            if (!url.isValid() || !url.isRelative() || !url.host().isEmpty()) {
                respond(socket, {"400 Bad Request", "text/plain", "Invalid URL", {}});
                return;
            }
            if (url.path() == "/" || url.path() == "/app.js") {
                QFile file(url.path() == "/" ? ":/viewer/index.html" : ":/viewer/app.js");
                if (!file.open(QIODevice::ReadOnly)) {
                    respond(socket, {"500 Server Error", "text/plain", "Viewer unavailable", {}});
                    return;
                }
                respond(socket, {"200 OK", url.path() == "/" ? "text/html; charset=utf-8" : "text/javascript",
                                 file.readAll(), {}});
                return;
            }
            if (url.path() != "/api/sessions" && url.path() != "/api/view") {
                respond(socket, {"404 Not Found", "text/plain", "Page unavailable", {}});
                return;
            }
            QPointer<QSslSocket> guard(socket);
            if (!readers_.tryStart([this, guard, url] {
                    auto reader = acquireReader();
                    const auto result = query(*reader, url);
                    releaseReader(std::move(reader));
                    QMetaObject::invokeMethod(this, [guard, result] {
                        if (guard && guard->state() == QAbstractSocket::ConnectedState)
                            respond(guard, result);
                    });
                }))
                respond(socket, {"503 Service Unavailable", "text/plain", "All readers busy. Try again.", {}});
        });
        socket->startServerEncryption();
    }

    QString database_;
    QByteArray expected_;
    QSslConfiguration ssl_;
    int active_ = 0;
    QHash<QString, int> activeByIp_;
    QHash<QString, AuthWindow> authFailures_;
    QElapsedTimer clock_; // A wall-clock step must not stretch or cut short a limit.
    std::mutex poolMutex_;
    std::vector<std::unique_ptr<History>> pool_;
    QThreadPool readers_;
};
} // namespace

std::unique_ptr<QTcpServer> startReceiverViewer(quint16 port, const QByteArray &password,
                                                const QString &database, const QHostAddress &address,
                                                const QSslConfiguration &ssl) {
    auto viewer = std::make_unique<Viewer>(database, password, ssl);
    if (!viewer->listen(address, port))
        throw Error(viewer->errorString());
    log(QString("Read-only HTTPS viewer listening on port %1").arg(port));
    return viewer;
}
} // namespace telemetry
