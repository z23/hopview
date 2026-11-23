#include "nodeconfig.h"
#include "history.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <cmath>

namespace telemetry {
namespace {
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
bool tokenMatches(const QByteArray &candidate, const QByteArray &expectedDigest) {
    const auto digest = QCryptographicHash::hash(candidate, QCryptographicHash::Sha256);
    unsigned mismatch = 0;
    for (int i = 0; i < digest.size(); ++i)
        mismatch |= static_cast<unsigned char>(digest[i] ^ expectedDigest[i]);
    return mismatch == 0;
}

class Receiver final : public QTcpServer {
  public:
    Receiver(History &history, QHash<QString, QByteArray> tokens, bool plain, QSslConfiguration ssl)
        : history_(history), tokens_(std::move(tokens)), plain_(plain), ssl_(std::move(ssl)) {}

  private:
    void incomingConnection(qintptr descriptor) override {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        if (clients_ >= 64) {
            delete socket;
            return;
        }
        ++clients_;
        connect(socket, &QObject::destroyed, this, [this] { --clients_; });
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        deadline->start(10000);
        socket->setReadBufferSize(maxFrame + 1);
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, deadline, site = QString()]() mutable {
            if (!plain_ && !socket->isEncrypted()) {
                socket->abort();
                return;
            }
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() >= maxFrame)
                    socket->abort();
                return;
            }
            auto bytes = socket->readLine(maxFrame + 1);
            if (bytes.size() > maxFrame || !bytes.endsWith('\n') || socket->bytesAvailable()) {
                socket->abort();
                return;
            }
            const auto received = now();
            try {
                const auto message = QJsonDocument::fromJson(bytes).object();
                if (message.value("v").toInt(-1) != 1)
                    throw Error("Unsupported protocol");
                const auto seq = integer(message, "seq", 0, 9007199254740991LL);
                const auto type = message.value("type").toString();
                if (site.isEmpty()) {
                    const auto claim = message.value("site").toString();
                    auto it = tokens_.constFind(claim);
                    if (type != "hello" || seq != 0 || it == tokens_.cend() ||
                        !tokenMatches(message.value("token").toString().toLatin1(), it.value()))
                        throw Error("Authentication failed");
                    site = claim;
                } else if (type == "batch" && seq > 0) {
                    store(site, message.value("body").toObject(), received);
                } else if (type != "ping" || seq != 0) {
                    throw Error("Unexpected message");
                }
                const QJsonObject ack{{"v", 1}, {"ack", seq}, {"receivedMs", received}, {"sentMs", now()}};
                socket->write(QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n');
                deadline->start(45000);
            } catch (const std::exception &e) {
                const QJsonObject failure{{"v", 1}, {"error", QString::fromUtf8(e.what())}};
                socket->write(QJsonDocument(failure).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromHost();
            }
        });
        if (!plain_) {
            socket->setSslConfiguration(ssl_);
            socket->startServerEncryption();
        }
    }

    void store(const QString &site, const QJsonObject &body, qint64 received) {
        const auto trace = parseTrace(body.value("trace").toObject());
        auto address = body.value("resolvedAddress").toString();
        const auto ip = QHostAddress(address);
        if (!validText(address, 128) || ip.isNull() ||
            (trace.family == 4 && ip.protocol() != QAbstractSocket::IPv4Protocol) ||
            (trace.family == 6 && ip.protocol() != QAbstractSocket::IPv6Protocol))
            throw Error("Invalid resolved address");
        const int family = ip.protocol() == QAbstractSocket::IPv4Protocol ? 4 : 6;
        const auto offset = integer(body, "clockOffsetMs", -maxStamp, maxStamp);
        const auto uncertainty = integer(body, "clockErrorMs", -1, 60000);
        const auto array = body.value("samples").toArray();
        if (array.isEmpty() || array.size() > 2048)
            throw Error("Invalid sample count");
        qint64 latestSample = 0;
        QVector<Sample> samples;
        samples.reserve(array.size());
        for (const auto &value : array) {
            const auto row = value.toArray();
            if (row.size() != 5)
                throw Error("Invalid sample");
            const qint64 t = row[0].toInteger(-1);
            int hop = row[1].toInt(-1);
            const auto responder = row[2].toString();
            const auto rtt = row[3].toDouble(-2);
            if (t < 0 || t > maxStamp || hop < 1 || hop > 255 || !row[2].isString() ||
                !validText(responder, 128, true) ||
                (!responder.isEmpty() && QHostAddress(responder).isNull()) || !std::isfinite(rtt) ||
                !(rtt == -1 || (rtt >= 0 && rtt <= 60000)) || responder.isEmpty() != (rtt < 0) ||
                !row[4].isBool() || (row[4].toBool() && responder.isEmpty()))
                throw Error("Invalid probe result");
            samples.append({0, t, hop, responder, rtt, row[4].toBool()});
            latestSample = std::max(latestSample, t);
        }
        // A DNS change creates a distinct stream, so comparisons can always identify the actual endpoint.
        auto identity = traceObject(trace);
        identity["address"] = ip.toString();
        const auto key = QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                .toHex());
        const auto session =
            history_.receiverSession(site, key, trace.target, family, trace.intervalMs, trace.probes.protocol,
                                     trace.probes.port, trace.probes.interfaceName);
        if (!session)
            throw Error(history_.error());
        for (auto &sample : samples)
            sample.session = session;
        if (!history_.append(samples) ||
            !history_.setSessionOrigin(session, site, address, offset, uncertainty, received, latestSample))
            throw Error(history_.error());
    }

    History &history_;
    QHash<QString, QByteArray> tokens_;
    bool plain_;
    QSslConfiguration ssl_;
    int clients_ = 0;
};
} // namespace

int receive(const QJsonObject &config, const QString &base) {
    const auto host = config.value("listen").toString("127.0.0.1");
    const auto port = config.contains("port") ? integer(config, "port", 1, 65535) : defaultPort;
    const auto plain = plainLoopback(config, host);
    QSslConfiguration ssl;
    if (!plain)
        ssl = tlsConfig(config, base, true);
    QHash<QString, QByteArray> tokens;
    const auto sites = config.value("sites").toObject();
    for (auto it = sites.begin(); it != sites.end(); ++it) {
        if (!validSite(it.key()))
            throw Error("Invalid site identifier in receiver configuration");
        const auto token = readToken(filePath(base, sites, it.key()));
        const auto digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
        if (tokens.values().contains(digest))
            throw Error("Each site requires a distinct token");
        tokens.insert(it.key(), digest);
    }
    if (tokens.isEmpty())
        throw Error("Configure at least one site and token file");
    History history(filePath(base, config, "database"));
    if (!history.open())
        throw Error(history.error());
    Receiver server(history, tokens, plain, ssl);
    QHostAddress bind;
    if (!bind.setAddress(host))
        throw Error("Receiver listen must be a numeric IP address");
    if (!server.listen(bind, port))
        throw Error(server.errorString());
    log(QString("Receiving telemetry on %1 port %2 (%3)")
            .arg(host)
            .arg(port)
            .arg(plain ? "loopback" : "TLS"));
    return QCoreApplication::exec();
}
} // namespace telemetry
