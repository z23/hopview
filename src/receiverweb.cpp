#include "receiverweb.h"
#include "history.h"
#include "nodeconfig.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSslSocket>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>

namespace telemetry {
namespace {
struct Response {
    QByteArray status, type, body;
};

Response query(const QString &database, const QUrl &url) {
    History history(database);
    if (!history.openReadOnly())
        return {"503 Service Unavailable", "text/plain", "History unavailable"};
    if (url.path() == "/api/sessions") {
        const auto sessions = history.sessions(true);
        if (!history.error().isEmpty())
            return {"503 Service Unavailable", "text/plain", "History unavailable"};
        return {"200 OK", "application/json",
                QJsonDocument(QJsonArray::fromVariantList(sessions)).toJson(QJsonDocument::Compact)};
    }
    const QUrlQuery params(url);
    bool idOk, fromOk, throughOk;
    const auto id = params.queryItemValue("session").toLongLong(&idOk);
    const auto from = params.queryItemValue("from").toLongLong(&fromOk);
    const auto through = params.queryItemValue("through").toLongLong(&throughOk);
    if (!idOk || !fromOk || !throughOk || id < 1 || from < 0 || from > through || through > maxStamp ||
        through - from > 31LL * 86400000)
        return {"400 Bad Request", "text/plain", "Choose a recording and up to 31 days of UTC history"};
    const auto sessions = history.sessions(true, id);
    if (!history.error().isEmpty())
        return {"503 Service Unavailable", "text/plain", "History unavailable"};
    if (sessions.isEmpty())
        return {"404 Not Found", "text/plain", "Remote recording unavailable"};
    auto view = history.view(id, from, through, from, through, {0}, 720);
    if (!history.error().isEmpty())
        return {"503 Service Unavailable", "text/plain", "History unavailable"};
    return {"200 OK", "application/json",
            QJsonDocument(QJsonObject::fromVariantMap(view)).toJson(QJsonDocument::Compact)};
}

void respond(QSslSocket *socket, const Response &r) {
    QByteArray headers = "HTTP/1.1 " + r.status + "\r\nContent-Type: " + r.type +
                         "\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n"
                         "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; object-src "
                         "'none'; frame-ancestors 'none'\r\n";
    if (r.status.startsWith("401"))
        headers += "WWW-Authenticate: Basic realm=\"Hopview reader\"\r\n";
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
    }

  private:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor) || active_ >= 32) {
            delete socket;
            return;
        }
        ++active_;
        connect(socket, &QObject::destroyed, this, [this] { --active_; });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(15000, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        socket->setSslConfiguration(ssl_);
        socket->setReadBufferSize(8193);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket] {
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
            const auto lines = bytes.first(end).split('\n');
            const auto request = lines.first().trimmed().split(' ');
            QByteArray credentials;
            for (const auto &line : lines) {
                const auto split = line.indexOf(':');
                if (split > 0 && line.first(split).compare("Authorization", Qt::CaseInsensitive) == 0) {
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
                respond(socket, {"401 Unauthorized", "text/plain", "Viewer credentials required"});
                return;
            }
            if (request.size() != 3 || request.first() != "GET") {
                respond(socket, {"405 Method Not Allowed", "text/plain", "Use GET"});
                return;
            }
            const auto url = QUrl::fromEncoded(request.at(1), QUrl::StrictMode);
            if (!url.isValid() || !url.isRelative() || !url.host().isEmpty()) {
                respond(socket, {"400 Bad Request", "text/plain", "Invalid URL"});
                return;
            }
            if (url.path() == "/" || url.path() == "/app.js") {
                QFile file(url.path() == "/" ? ":/viewer/index.html" : ":/viewer/app.js");
                if (!file.open(QIODevice::ReadOnly)) {
                    respond(socket, {"500 Server Error", "text/plain", "Viewer unavailable"});
                    return;
                }
                respond(socket, {"200 OK", url.path() == "/" ? "text/html; charset=utf-8" : "text/javascript",
                                 file.readAll()});
                return;
            }
            if (url.path() != "/api/sessions" && url.path() != "/api/view") {
                respond(socket, {"404 Not Found", "text/plain", "Page unavailable"});
                return;
            }
            QPointer<QSslSocket> guard(socket);
            if (!readers_.tryStart([this, guard, url] {
                    const auto result = query(database_, url);
                    QMetaObject::invokeMethod(this, [guard, result] {
                        if (guard && guard->state() == QAbstractSocket::ConnectedState)
                            respond(guard, result);
                    });
                }))
                respond(socket, {"503 Service Unavailable", "text/plain", "All readers busy. Try again."});
        });
        socket->startServerEncryption();
    }

    QString database_;
    QByteArray expected_;
    QSslConfiguration ssl_;
    int active_ = 0;
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
