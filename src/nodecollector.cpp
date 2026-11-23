#include "nodeconfig.h"
#include "probe.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QSet>
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
using namespace hopline_sql;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

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
                "CREATE TABLE IF NOT EXISTS destination(name TEXT NOT NULL);");
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
        token_ = QString::fromLatin1(readToken(filePath(base, config, "tokenFile")));
        if (!plain_)
            ssl_ = tlsConfig(config, base, false);
        const auto targets = config.value("targets").toArray();
        if (targets.isEmpty() || targets.size() > 128)
            throw Error("Configure 1 through 128 targets per collector");
        QSet<QByteArray> seen;
        for (const auto &obj : targets) {
            auto t = parseTrace(obj.toObject());
            const auto key = QJsonDocument(traceObject(t)).toJson(QJsonDocument::Compact);
            if (seen.contains(key))
                throw Error("Duplicate trace configuration");
            seen.insert(key);
            traces_.append(t);
        }
        probe_ = new Probe;
        probe_->moveToThread(&probeThread_);
        connect(&probeThread_, &QThread::finished, probe_, &QObject::deleteLater);
        connect(probe_, &Probe::resolved, this,
                [this](qint64 id, const QString &address, const QString &, int) {
                    addresses_.insert(id, address);
                });
        // Slow fsync and receiver backfill must leave the probe timer independent of storage latency.
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
            log(QString("%1: %2").arg(traces_[id - 1].target, message));
            if (failed)
                QTimer::singleShot(30000, this, [this, id] { startProbe(id); });
        });
        probeThread_.start();
        reconnect_.setSingleShot(true);
        connect(&reconnect_, &QTimer::timeout, this, [this] { connectReceiver(); });
        deadline_.setSingleShot(true);
        connect(&deadline_, &QTimer::timeout, this, [this] { lost("Receiver timed out"); });
        ping_.setInterval(15000);
        connect(&ping_, &QTimer::timeout, this, [this] {
            if (ready_ && seq_ < 0)
                send({{"type", "ping"}}, 0);
        });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            stopProbes();
            QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        });
    }

    ~Collector() override { stopProbes(); }

    void start() {
        for (int i = 0; i < traces_.size(); ++i)
            startProbe(i + 1);
        connectReceiver();
        ping_.start();
    }

  private:
    void fatal(const std::exception &e) {
        log("Collector storage error: " + QString::fromUtf8(e.what()));
        QCoreApplication::exit(1);
    }
    void startProbe(qint64 id) {
        if (!probe_)
            return;
        auto t = traces_[id - 1];
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
        const auto &t = traces_[id - 1];
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
        socket->setReadBufferSize(4097);
        auto hello = [this] { send({{"type", "hello"}, {"site", site_}, {"token", token_}}, 0); };
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
        const auto record = spool_.oldest();
        if (record.first)
            send({{"type", "batch"}, {"body", record.second}}, record.first);
    }

    void readAck() {
        if ((!plain_ && !socket_->isEncrypted()) || seq_ < 0 || socket_->bytesAvailable() > 4096) {
            lost("Invalid receiver response");
            return;
        }
        if (!socket_->canReadLine())
            return;
        const auto ack = QJsonDocument::fromJson(socket_->readLine(4096)).object();
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

    QString site_, host_, token_;
    int port_;
    bool plain_;
    Spool &spool_;
    QSslConfiguration ssl_;
    QThread probeThread_;
    Probe *probe_ = nullptr;
    std::atomic<qint64> queuedSamples_{0};
    std::atomic_bool overloaded_{false};
    QList<Trace> traces_;
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
    if (!validSite(site) || !validText(host, 255))
        throw Error("Configure a site and receiver hostname");
    const auto port = config.contains("port") ? int(integer(config, "port", 1, 65535)) : defaultPort;
    auto plain = plainLoopback(config, host);
    const auto bound = config.contains("spoolMiB") ? integer(config, "spoolMiB", 1, 4096) : 64;
    const auto dest = QString("%1/%2/%3/%4").arg(site, host).arg(port).arg(plain ? "loopback" : "tls");
    Spool spool(filePath(base, config, "spool"), dest, bound * 1024 * 1024);
    Collector client(config, base, site, host, port, plain, spool);
    QTimer::singleShot(0, &client, [&client] { client.start(); });
    return QCoreApplication::exec();
}
} // namespace telemetry
