#include "nodeconfig.h"
#include "history.h"
#include "receiverweb.h"
#include "collectormanagement.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QThreadPool>
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
    Receiver(History &history, CollectorManagement &management, QHash<QString, QByteArray> tokens,
             bool plain, QSslConfiguration ssl)
        : history_(history), management_(management), tokens_(std::move(tokens)), plain_(plain), ssl_(std::move(ssl)) {}

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
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, deadline, site = QString(), managed = false]() mutable {
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
                QJsonObject ack;
                if (site.isEmpty()) {
                    if (type != "hello" || seq != 0)
                        throw Error("Authentication failed");
                    managed = message.value("managed").toBool();
                    if (managed) {
                        if (plain_)
                            throw Error("Managed enrollment requires TLS");
                        ack = management_.enroll(message.value("token").toString(), message.value("nodeId").toString(),
                                                 message.value("hostname").toString(), tokens_.keys(),
                                                 integer(message, "appliedRevision", 0, 9007199254740991LL),
                                                 message.value("applyError").toString(), received);
                        site = ack.value("site").toString();
                    } else {
                        const auto claim = message.value("site").toString();
                        auto it = tokens_.constFind(claim);
                        if (it == tokens_.cend() ||
                            !tokenMatches(message.value("token").toString().toLatin1(), it.value()) ||
                            management_.hasSite(claim))
                            throw Error("Authentication failed");
                        site = claim;
                    }
                    ack["multiBatch"] = true;
                } else if (type == "batches" && seq > 0) {
                    if (managed && !management_.hasSite(site))
                        throw Error("Collector access revoked");
                    const auto bodies = message["bodies"].toArray();
                    if (bodies.isEmpty() || bodies.size() > 32)
                        throw Error("Invalid telemetry batch group");
                    // Commit the full batch group in one WAL transaction; each record stays idempotent on retry.
                    QHash<QPair<QString, QString>, qint64> created;
                    if (!history_.withWriteTransaction([&] {
                            for (const auto &body : bodies)
                                store(site, body.toObject(), received, created);
                        }))
                        throw Error(history_.error());
                    cacheSessions(created);
                } else if (type == "batch" && seq > 0) {
                    if (managed && !management_.hasSite(site))
                        throw Error("Collector access revoked");
                    QHash<QPair<QString, QString>, qint64> created;
                    if (!history_.withWriteTransaction([&] {
                            store(site, message.value("body").toObject(), received, created);
                        }))
                        throw Error(history_.error());
                    cacheSessions(created);
                } else if (type == "ping" && seq == 0) {
                    if (managed)
                        ack = management_.heartbeat(site, integer(message, "appliedRevision", 0, 9007199254740991LL),
                                                    message.value("applyError").toString(), received);
                } else {
                    throw Error("Unexpected message");
                }
                if ((type == "hello" || type == "ping") && message.contains("health"))
                    management_.saveHealth(site, message["health"].toObject(), received);
                ack["v"] = 1;
                ack["ack"] = seq;
                ack["receivedMs"] = received;
                ack["sentMs"] = now();
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

    void cacheSessions(const QHash<QPair<QString, QString>, qint64> &created) {
        if (created.isEmpty())
            return;
        if (sessions_.size() + created.size() > 8192)
            sessions_.clear();
        for (auto it = created.cbegin(); it != created.cend(); ++it)
            sessions_.insert(it.key(), it.value());
    }

    void store(const QString &site, const QJsonObject &body, qint64 received,
               QHash<QPair<QString, QString>, qint64> &created) {
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
        auto array = body.value("samples").toArray();
        const auto legacyCount = array.size();
        for (const auto &value : body["observations"].toArray()) array.append(value);
        auto measurement = body["measurement"].toObject();
        if (!measurement.isEmpty() && (!validMeasurement(measurement) || measurement["resolvedAddress"].toString() != address))
            throw Error("Invalid measurement settings");
        const auto diagnostic = body["diagnostic"].toObject();
        if ((array.isEmpty() && diagnostic.isEmpty()) || array.size() > 2048)
            throw Error("Invalid sample count");
        qint64 latestSample = diagnostic["t"].toInteger(0);
        QVector<Sample> samples;
        samples.reserve(array.size());
        for (int index = 0; index < array.size(); ++index) {
            const bool detailed = index >= legacyCount;
            const auto row = array[index].toArray();
            ProbeFailure failure;
            if (row.size() != (detailed ? 6 : 5) ||
                (detailed && (!row.at(5).isObject() || (!row.at(5).toObject().isEmpty() && !ProbeFailure::read(row.at(5).toObject(),failure)))))
                throw Error("Invalid sample");
            const qint64 t = row[0].toInteger(-1);
            int hop = row[1].toInt(-1);
            const auto responder = row[2].toString();
            const auto rtt = row[3].toDouble(-2);
            if (t < 0 || t > maxStamp || hop < (detailed ? 0 : 1) || hop > 255 || !row[2].isString() ||
                !validText(responder, 128, true) ||
                (!responder.isEmpty() && QHostAddress(responder).isNull()) || !std::isfinite(rtt) ||
                !(rtt == -1 || (rtt == -2 && failure.kind == "local-unsent") || (rtt >= 0 && rtt <= 60000)) || responder.isEmpty() != (rtt < 0) ||
                !row[4].isBool() || (row[4].toBool() && responder.isEmpty()))
                throw Error("Invalid probe result");
            samples.append({0, t, hop, responder, rtt, row[4].toBool(),failure});
            latestSample = std::max(latestSample, t);
        }
        // A DNS change creates a distinct stream, so comparisons can always identify the actual endpoint.
        auto identity = traceObject(trace);
        identity.remove("diagnostics");
        identity["address"] = ip.toString();
        const auto key = QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                .toHex());
        const auto cacheKey = qMakePair(site, key);
        qint64 session = sessions_.value(cacheKey, created.value(cacheKey, 0));
        if (!session) {
            session = history_.receiverSession(site, key, trace.target, family, trace.intervalMs,
                                               trace.probes.protocol, trace.probes.port, trace.probes.interfaceName,
                                               trace.probes.payloadBytes, trace.probes.dscp, trace.probes.dontFragment);
            if (!session)
                throw Error(history_.error());
            created.insert(cacheKey, session);
        }
        if (!measurement.isEmpty()) {
            measurement["clockOffsetMs"] = offset;
            measurement["clockErrorMs"] = uncertainty;
            auto earliest = latestSample;
            for (const auto &s : samples) earliest = std::min(earliest,s.timeMs);
            if (!history_.setMeasurement(session, earliest, measurement.toVariantMap())) throw Error(history_.error());
        }
        for (auto &sample : samples)
            sample.session = session;
        if ((!diagnostic.isEmpty() && !history_.appendDiagnostic(session, diagnostic.toVariantMap())) ||
            !history_.append(samples) ||
            !history_.setSessionOrigin(session, site, address, offset, uncertainty, received, latestSample))
            throw Error(history_.error());
    }

    History &history_;
    CollectorManagement &management_;
    QHash<QString, QByteArray> tokens_;
    QHash<QPair<QString, QString>, qint64> sessions_;
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
    const auto database = filePath(base, config, "database");
    History history(database);
    if (!history.open())
        throw Error(history.error());
    if (config.contains("retention")) {
        const auto retention=config["retention"].toObject();
        if (!history.setRetention(integer(retention,"rawDays",0,36500),integer(retention,"summaryDays",0,36500)))
            throw Error(history.error());
    }
    QThreadPool maintenancePool;
    maintenancePool.setMaxThreadCount(1);
    QTimer maintenance;
    const auto maintain = [&] {
        if (maintenancePool.activeThreadCount()) return;
        maintenancePool.start([database] {
            History store(database);
            if (!store.maintainStorage(now())) log("Storage maintenance: " + store.error());
        });
    };
    QObject::connect(&maintenance,&QTimer::timeout,&maintenance,maintain);
    maintenance.start(3600000);
    maintain();
    CollectorManagement management(database);
    Receiver server(history, management, tokens, plain, ssl);
    QHostAddress bind;
    if (host == "*")
        bind = QHostAddress::Any;
    else if (!bind.setAddress(host))
        throw Error("Receiver listen must be a numeric IP address or *");
    if (!server.listen(bind, port))
        throw Error(server.errorString());
    std::unique_ptr<QTcpServer> viewer;
    if (config.contains("viewer")) {
        if (plain) throw Error("The remote viewer requires a TLS receiver");
        const auto settings = config.value("viewer").toObject();
        const auto viewerPort = integer(settings, "port", 1, 65535);
        const auto password = readToken(filePath(base, settings, "tokenFile"));
        const auto digest = QCryptographicHash::hash(password, QCryptographicHash::Sha256);
        if (password.startsWith("hv1.") || tokens.values().contains(digest) || management.hasKeyDigest(digest))
            throw Error("Choose a viewer password separate from collector credentials");
        viewer = startReceiverViewer(viewerPort, password, database, bind, ssl);
    }
    const auto fingerprint = ssl.localCertificate().digest(QCryptographicHash::Sha256).toHex();
    management.setCertificateFingerprint(plain ? QString{} : QString::fromLatin1(fingerprint));
    log(QString("Receiving telemetry on %1 port %2 (%3)")
            .arg(host)
            .arg(port)
            .arg(plain ? "loopback" : "TLS"));
    return QCoreApplication::exec();
}
} // namespace telemetry
