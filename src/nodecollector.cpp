#include "nodeconfig.h"
#include "nodejoin.h"
#include "probe.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSet>
#include <QCryptographicHash>
#include <QHostInfo>
#include <QUuid>
#include <QThread>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSslSocket>
#include <QTimer>
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <utility>
#include <cstdlib>
#include <memory>

namespace telemetry {
namespace {
using namespace hopview_sql;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

QByteArray identity(const Trace &trace) {
    return QJsonDocument(traceObject(trace)).toJson(QJsonDocument::Compact);
}

QList<Trace> readTargets(const QJsonValue &value, bool allowEmpty) {
    if (!value.isArray() || (!allowEmpty && value.toArray().isEmpty()) || value.toArray().size() > 128)
        throw Error(allowEmpty ? "Configure at most 128 targets per collector" :
                                 "Configure 1 through 128 targets per collector");
    QList<Trace> result;
    QSet<QByteArray> seen;
    for (const auto &target : value.toArray()) {
        const auto trace = parseTrace(target.toObject());
        const auto key = identity(trace);
        if (seen.contains(key))
            throw Error("Duplicate trace configuration");
        seen.insert(key);
        result.append(trace);
    }
    return result;
}

class Spool {
  public:
    Spool(const QString &path, const QString &destination, qint64 limit)
        : lock_(path + ".lock"), limit_(limit) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            throw Error("Cannot create spool directory");
        lock_.setStaleLockTime(0);
        if (!lock_.tryLock())
            throw Error("Cannot acquire collector spool " + path);
        sqlite3 *opened = nullptr;
        const auto rc = sqlite3_open_v2(QFile::encodeName(path).constData(), &opened,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        db_.reset(opened);
        if (rc != SQLITE_OK)
            throw Error("Cannot open collector spool " + path);
        auto db = db_.get();
        execute(db,
                "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
                "CREATE TABLE IF NOT EXISTS outbox(id INTEGER PRIMARY KEY AUTOINCREMENT,body TEXT NOT NULL);"
                "CREATE TABLE IF NOT EXISTS destination(name TEXT NOT NULL);"
                "CREATE TABLE IF NOT EXISTS assignment(id INTEGER PRIMARY KEY CHECK(id=1),site TEXT NOT NULL,"
                "revision INTEGER NOT NULL,targets TEXT NOT NULL);");
        Transaction tx(db, true);
        Statement origin(db, "SELECT name FROM destination LIMIT 1");
        if (origin.next()) {
            if (origin.string(0) != destination)
                throw Error("Spool already belongs to a different receiver/site");
        } else {
            Statement set(db, "INSERT INTO destination VALUES(?)");
            set.string(1, destination);
            set.next();
        }
        Statement size(db, "SELECT coalesce(sum(length(cast(body AS blob))),0) FROM outbox");
        size.next();
        bytes_ = size.integer(0);
        tx.commit();
    }

    QJsonObject assignment() const {
        Statement row(db_.get(), "SELECT site,revision,targets FROM assignment WHERE id=1");
        if (!row.next())
            return {};
        return {{"site", row.string(0)}, {"revision", row.integer(1)},
                {"targets", QJsonDocument::fromJson(row.string(2).toUtf8()).array()}};
    }
    void saveAssignment(const QString &site, qint64 revision, const QJsonArray &targets) {
        Statement save(db_.get(), "INSERT OR REPLACE INTO assignment(id,site,revision,targets) VALUES(1,?,?,?)");
        save.string(1, site);
        save.integer(2, revision);
        save.string(3, QString::fromUtf8(QJsonDocument(targets).toJson(QJsonDocument::Compact)));
        save.next();
    }
    void push(const QJsonObject &body) {
        const auto json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        if (json.size() > maxFrame - 1024)
            throw Error("Probe batch exceeds the telemetry frame size");
        auto db = db_.get();
        Transaction tx(db, true);
        Statement insert(db, "INSERT INTO outbox(body) VALUES(?)");
        insert.string(1, QString::fromUtf8(json));
        insert.next();
        auto total = bytes_ + json.size();
        int dropped = 0;
        while (total > limit_) {
            Statement oldest(db, "SELECT id,length(cast(body AS blob)) FROM outbox ORDER BY id LIMIT 1");
            if (!oldest.next())
                throw Error("Invalid spool accounting");
            auto id = oldest.integer(0);
            total -= oldest.integer(1);
            Statement remove(db, "DELETE FROM outbox WHERE id=?");
            remove.integer(1, id);
            remove.next();
            ++dropped;
        }
        tx.commit();
        bytes_ = total;
        if (dropped) {
            drops_ += dropped;
            if (!dropLog_.isValid() || dropLog_.elapsed() >= 60000) {
                log(QString("Collector spool at capacity. Dropped %1 oldest unsent batch(es) since startup")
                        .arg(drops_));
                dropLog_.start();
            }
        }
    }

    QPair<qint64, QJsonObject> oldest() const {
        Statement next(db_.get(), "SELECT id,body FROM outbox ORDER BY id LIMIT 1");
        if (!next.next())
            return {};
        return {next.integer(0), QJsonDocument::fromJson(next.string(1).toUtf8()).object()};
    }
    void confirm(qint64 id) {
        auto db = db_.get();
        Transaction tx(db, true);
        Statement item(db, "SELECT length(cast(body AS blob)) FROM outbox WHERE id=?");
        item.integer(1, id);
        const auto size = item.next() ? item.integer(0) : 0;
        Statement del(db, "DELETE FROM outbox WHERE id=?");
        del.integer(1, id);
        del.next();
        tx.commit();
        bytes_ -= size;
    }

  private:
    QLockFile lock_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
    qint64 limit_, bytes_ = 0, drops_ = 0;
    QElapsedTimer dropLog_;
};

class Collector final : public QObject {
  public:
    Collector(const QJsonObject &config, const QString &base, const QString &site, const QString &host,
              int port, bool plain, Spool &spool)
        : site_(site), host_(host), port_(port), plain_(plain), spool_(spool) {
        managed_ = config.value("managed").toBool();
        if (managed_) {
            nodeId_ = config.value("nodeId").toString();
            if (QUuid(nodeId_).isNull())
                throw Error("Missing managed collector identity");
            const auto key = parseEnrollmentKey(config.value("enrollmentKey").toString());
            token_ = key.secret;
            pin_ = key.certificateDigest;
            const auto cached = spool_.assignment();
            if (!cached.isEmpty()) {
                site_ = cached["site"].toString();
                applied_ = integer(cached, "revision", 1, 9007199254740991LL);
                installTargets(readTargets(cached["targets"], true));
            }
        } else {
            token_ = QString::fromLatin1(readToken(filePath(base, config, "tokenFile")));
            installTargets(readTargets(config.value("targets"), false));
        }
        if (!plain_)
            ssl_ = tlsConfig(config, base, false);
        probe_ = new Probe;
        probe_->moveToThread(&probeThread_);
        connect(&probeThread_, &QThread::finished, probe_, &QObject::deleteLater);
        connect(probe_, &Probe::resolved, this,
                [this](qint64 id, const QString &address, const QString &, int) {
                    if (traces_.contains(id))
                        addresses_.insert(id, address);
                });
        // Disk flushes and receiver backfill run independently of the probe timer.
        connect(probe_, &Probe::samples, probe_, [this](QVector<Sample> samples) {
            if (samples.isEmpty())
                return;
            const auto count = samples.size();
            if (queuedSamples_.fetch_add(count) + count > 65536) {
                queuedSamples_.fetch_sub(count);
                if (!overloaded_.exchange(true))
                    QMetaObject::invokeMethod(this, [] {
                        log("Collector storage fell behind. Some observations could not be queued.");
                        QCoreApplication::exit(1);
                    });
                return;
            }
            QMetaObject::invokeMethod(this, [this, samples = std::move(samples), count] {
                try {
                    enqueue(samples);
                    pump();
                } catch (const std::exception &e) {
                    fatal(e);
                }
                queuedSamples_.fetch_sub(count);
            });
        });
        connect(probe_, &Probe::status, this, [this](qint64 id, const QString &message, bool failed) {
            if (!traces_.contains(id))
                return;
            log(QString("%1: %2").arg(traces_.value(id).target, message));
            if (failed)
                QTimer::singleShot(30000, this, [this, id] { startProbe(id); });
        });
        probeThread_.start();
        reconnect_.setSingleShot(true);
        connect(&reconnect_, &QTimer::timeout, this, [this] { connectReceiver(); });
        deadline_.setSingleShot(true);
        connect(&deadline_, &QTimer::timeout, this, [this] { lost("Receiver timed out"); });
        ping_.setInterval(managed_ ? 5000 : 15000);
        connect(&ping_, &QTimer::timeout, this, [this] {
            pingPending_ = true;
            pump();
        });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            stopProbes();
            QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        });
    }

    ~Collector() override { stopProbes(); }

    void start() {
        for (auto id : active_)
            startProbe(id);
        connectReceiver();
        ping_.start();
    }

  private:
    void fatal(const std::exception &e) {
        log("Collector storage error: " + QString::fromUtf8(e.what()));
        QCoreApplication::exit(1);
    }
    void installTargets(const QList<Trace> &targets) {
        QHash<QByteArray, qint64> next;
        QList<qint64> added, retired;
        for (const auto &target : targets) {
            const auto key = identity(target);
            auto id = active_.value(key);
            if (!id) {
                id = ++nextId_;
                traces_.insert(id, target);
                added.append(id);
            }
            next.insert(key, id);
        }
        for (auto it = active_.cbegin(); it != active_.cend(); ++it)
            if (!next.contains(it.key()))
                retired.append(it.value());
        active_ = std::move(next);
        if (!probe_)
            return;
        for (const auto id : retired)
            QMetaObject::invokeMethod(probe_, [probe = probe_, id] { probe->stop(id); });
        for (const auto id : added)
            startProbe(id);
        if (!added.isEmpty() || !retired.isEmpty()) {
            // Wait until the worker has installed the revision before acknowledging it.
            QMetaObject::invokeMethod(probe_, [] {}, Qt::BlockingQueuedConnection);
            // Keep retired trace metadata available to samples already queued for storage.
            QMetaObject::invokeMethod(this, [this, retired] {
                for (const auto id : retired) {
                    traces_.remove(id);
                    addresses_.remove(id);
                }
            }, Qt::QueuedConnection);
        }
    }
    void startProbe(qint64 id) {
        if (!probe_ || active_.key(id).isEmpty())
            return;
        const auto t = traces_.value(id);
        QMetaObject::invokeMethod(probe_, [probe = probe_, id, t] {
            probe->start(id, t.target, t.family, t.intervalMs, t.probes.protocol, t.probes.port,
                         t.probes.interfaceName);
        });
    }
    void stopProbes() {
        if (!probe_)
            return;
        QMetaObject::invokeMethod(probe_, &Probe::shutdown, Qt::BlockingQueuedConnection);
        probeThread_.quit();
        probeThread_.wait();
        probe_ = nullptr;
    }

    void enqueue(const QVector<Sample> &samples) {
        auto id = samples.front().session;
        const auto it = traces_.constFind(id);
        if (it == traces_.cend())
            return;
        const auto &t = it.value();
        // Expiry and wall-vs-monotonic checks prevent attaching a pre-step estimate to new observations.
        const auto age = estimateAge_.isValid() ? estimateAge_.elapsed() : -1;
        const bool validClock = age >= 0 && age <= 60000 && std::abs(now() - estimateAt_ - age) < 20 &&
                                std::abs(samples.front().timeMs - estimateAt_) <= 60000;
        auto body = QJsonObject{{"trace", traceObject(t)},
                                {"resolvedAddress", addresses_.value(id)},
                                {"clockOffsetMs", validClock ? offsetMs_ : 0},
                                {"clockErrorMs", validClock ? clockErrorMs_ : -1}};
        QJsonArray rows;
        for (const auto &s : samples)
            rows.append(QJsonArray{s.timeMs, s.hop, s.address, s.rttMs, s.destination});
        body["samples"] = rows;
        spool_.push(body);
    }

    void connectReceiver() {
        auto *socket = new QSslSocket(this);
        socket_ = socket;
        socket->setReadBufferSize(managed_ ? maxFrame + 1 : 4097);
        auto pinned = [this, socket] {
            const auto cert = socket->peerCertificate();
            const auto utc = QDateTime::currentDateTimeUtc();
            return !cert.isNull() && cert.digest(QCryptographicHash::Sha256) == pin_ &&
                   cert.effectiveDate() <= utc && utc <= cert.expiryDate();
        };
        if (!pin_.isEmpty()) {
            connect(socket, &QSslSocket::sslErrors, this, [socket, pinned](const QList<QSslError> &errors) {
                if (pinned() && std::all_of(errors.cbegin(), errors.cend(), [](const QSslError &e) {
                    switch (e.error()) {
                    case QSslError::SelfSignedCertificate:
                    case QSslError::SelfSignedCertificateInChain:
                    case QSslError::UnableToGetIssuerCertificate:
                    case QSslError::UnableToGetLocalIssuerCertificate:
                    case QSslError::UnableToVerifyFirstCertificate:
                    case QSslError::CertificateUntrusted:
                    case QSslError::HostNameMismatch:
                        return true;
                    default:
                        return false;
                    }
                }))
                    socket->ignoreSslErrors(errors);
            });
        }
        auto hello = [this, pinned] {
            if (!pin_.isEmpty() && !pinned()) {
                lost("Receiver certificate does not match the enrollment key");
                return;
            }
            QJsonObject message{{"type", "hello"}, {"token", token_}};
            if (managed_) {
                message["managed"] = true;
                message["nodeId"] = nodeId_;
                message["hostname"] = QHostInfo::localHostName().left(253);
                message["appliedRevision"] = applied_;
                message["applyError"] = applyError_;
            } else {
                message["site"] = site_;
            }
            send(message, 0);
        };
        connect(socket, &QSslSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
            if (socket_ == socket)
                lost(socket->errorString());
        });
        connect(socket, &QSslSocket::disconnected, this, [this, socket] {
            if (socket_ == socket)
                lost("Receiver disconnected");
        });
        connect(socket, &QSslSocket::readyRead, this, [this] { readAck(); });
        deadline_.start(10000);
        if (plain_) {
            connect(socket, &QSslSocket::connected, this, hello);
            socket->connectToHost(host_, port_);
        } else {
            connect(socket, &QSslSocket::encrypted, this, hello);
            socket->setSslConfiguration(ssl_);
            socket->connectToHostEncrypted(host_, port_);
        }
    }

    void lost(const QString &why) {
        if (!socket_)
            return;
        auto *old = socket_;
        socket_ = nullptr;
        disconnect(old, nullptr, this, nullptr);
        old->abort();
        old->deleteLater();
        deadline_.stop();
        ready_ = false;
        seq_ = -1;
        log(QString("Unable to upload telemetry (%1). Retrying in %2 s").arg(why).arg(backoff_ / 1000));
        reconnect_.start(backoff_);
        backoff_ = std::min(60000, backoff_ * 2);
    }

    void send(QJsonObject message, qint64 seq) {
        seq_ = seq;
        message["v"] = 1;
        message["seq"] = seq;
        auto line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
        sentWall_ = now();
        sentTimer_.start();
        if (socket_->write(line) != line.size()) {
            lost("Socket write failed");
            return;
        }
        deadline_.start(15000);
    }

    void pump() {
        if (!ready_ || seq_ >= 0)
            return;
        if (std::exchange(pingPending_, false)) {
            QJsonObject message{{"type", "ping"}};
            if (managed_) {
                message["appliedRevision"] = applied_;
                message["applyError"] = applyError_;
            }
            send(message, 0);
            return;
        }
        const auto record = spool_.oldest();
        if (record.first)
            send({{"type", "batch"}, {"body", record.second}}, record.first);
    }

    void readAck() {
        const auto limit = managed_ ? maxFrame : 4096;
        if ((!plain_ && !socket_->isEncrypted()) || seq_ < 0 || socket_->bytesAvailable() > limit) {
            lost("Invalid receiver response");
            return;
        }
        if (!socket_->canReadLine()) {
            if (socket_->bytesAvailable() >= limit)
                lost("Oversized acknowledgement");
            return;
        }
        const auto ack = QJsonDocument::fromJson(socket_->readLine(limit + 1)).object();
        const auto receivedWall = now(), elapsed = sentTimer_.elapsed();
        if (ack.value("v").toInt(-1) != 1 || ack.value("ack").toInteger(-1) != seq_) {
            lost(ack.contains("error") ? ack.value("error").toString().left(240) : "Invalid acknowledgement");
            return;
        }
        const auto t1 = ack.value("receivedMs").toInteger(-1), t2 = ack.value("sentMs").toInteger(-1);
        const auto processing = t2 - t1;
        if (t1 < 0 || t2 < t1 || t2 > maxStamp) {
            lost("Invalid receiver timestamps");
            return;
        }
        estimateAge_.invalidate();
        if (std::abs(receivedWall - sentWall_ - elapsed) <= 4 && elapsed <= 60000 &&
            processing <= elapsed + 2) {
            offsetMs_ = ((sentWall_ - t1) + (receivedWall - t2)) / 2;
            clockErrorMs_ = std::max<qint64>(0, elapsed - processing) / 2 + 2;
            estimateAt_ = receivedWall;
            estimateAge_.start();
        }
        deadline_.stop();
        const auto confirmed = std::exchange(seq_, -1);
        if (managed_ && confirmed == 0) {
            const auto site = ack["site"].toString();
            if (!validSite(site) || (!site_.isEmpty() && site_ != site)) {
                lost("Invalid site identity from receiver");
                return;
            }
            site_ = site;
            try {
                const auto revision = integer(ack, "revision", 1, 9007199254740991LL);
                if (revision != applied_) {
                    const auto targets = readTargets(ack["targets"], true);
                    spool_.saveAssignment(site, revision, ack["targets"].toArray());
                    installTargets(targets);
                    applied_ = revision;
                    pingPending_ = true;
                }
                applyError_.clear();
            } catch (const std::exception &e) {
                const auto reason = QString::fromUtf8(e.what()).left(200);
                if (reason != applyError_) {
                    pingPending_ = true;
                    log("Could not apply assigned targets: " + reason);
                }
                applyError_ = reason;
            }
        }
        if (!ready_)
            log("Telemetry connection established for " + site_);
        ready_ = true;
        backoff_ = 1000;
        try {
            if (confirmed > 0)
                spool_.confirm(confirmed);
            pump();
        } catch (const std::exception &e) {
            fatal(e);
        }
    }

    QString site_, host_, token_, nodeId_, applyError_;
    QByteArray pin_;
    qint64 applied_ = 0, nextId_ = 0;
    bool managed_ = false, pingPending_ = false;
    int port_;
    bool plain_;
    Spool &spool_;
    QSslConfiguration ssl_;
    QThread probeThread_;
    Probe *probe_ = nullptr;
    std::atomic<qint64> queuedSamples_{0};
    std::atomic_bool overloaded_{false};
    QHash<QByteArray, qint64> active_;
    QHash<qint64, Trace> traces_;
    QHash<qint64, QString> addresses_;
    QSslSocket *socket_ = nullptr;
    QTimer reconnect_, deadline_, ping_;
    QElapsedTimer sentTimer_, estimateAge_;
    qint64 sentWall_ = 0, estimateAt_ = 0, offsetMs_ = 0, clockErrorMs_ = -1, seq_ = -1;
    int backoff_ = 1000;
    bool ready_ = false;
};
} // namespace

int collect(const QJsonObject &config, const QString &base) {
    auto site = config.value("site").toString(), host = config.value("receiver").toString();
    const auto managed = config.value("managed").toBool();
    if ((!managed && !validSite(site)) || !validText(host, 255))
        throw Error("Configure a site and receiver hostname");
    const auto port = config.contains("port") ? int(integer(config, "port", 1, 65535)) : defaultPort;
    auto plain = plainLoopback(config, host);
    const auto bound = config.contains("spoolMiB") ? integer(config, "spoolMiB", 1, 4096) : 64;
    if (managed && plain)
        throw Error("Managed collectors require TLS");
    const auto identity = managed ? "managed:" + config.value("nodeId").toString() : site;
    const auto dest = QString("%1/%2/%3/%4").arg(identity, host).arg(port).arg(plain ? "loopback" : "tls");
    Spool spool(filePath(base, config, "spool"), dest, bound * 1024 * 1024);
    Collector client(config, base, site, host, port, plain, spool);
    QTimer::singleShot(0, &client, [&client] { client.start(); });
    return QCoreApplication::exec();
}
} // namespace telemetry
