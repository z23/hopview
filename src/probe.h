#pragma once

#include "types.h"

#include <QHash>
#include <QObject>
#include <atomic>

class ProbeSession;

class Probe : public QObject {
    Q_OBJECT
public:
    explicit Probe(QObject *parent = nullptr);
    void setPreviewSession(qint64 id) { previewSession_.store(id, std::memory_order_relaxed); }
    bool previews(qint64 id) const { return previewSession_.load(std::memory_order_relaxed) == id; }

public slots:
    void start(qint64 session, QString target, int family, int intervalMs, QString protocol = QStringLiteral("auto"),
                       int port = 0, QString interfaceName = {}, int payloadBytes = 16, int dscp = 0, bool dontFragment = false);
    void stop(qint64 session);
    void shutdown();
    void finish(qint64 session);

signals:
    void finished(qint64 session);
    void resolved(qint64 session, QString address, QString protocol, int port);
    void samples(QVector<Sample> batch);
    void measurement(qint64 session, qint64 stamp, QVariantMap actual);
    void replyReceived(qint64 session, qint64 roundMs, int hop, QString address, double rttMs);
    void status(qint64 session, QString message, bool fatal);

private:
    std::atomic<qint64> previewSession_{0};
    QHash<qint64, ProbeSession *> sessions_;
};
