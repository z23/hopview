#pragma once

#include "types.h"

#include <QHash>
#include <QObject>

class ProbeSession;

class Probe : public QObject {
    Q_OBJECT
public:
    explicit Probe(QObject *parent = nullptr);

public slots:
    void start(qint64 session, QString target, int family, int intervalMs);
    void stop(qint64 session);
    void shutdown();
    void finish(qint64 session);

signals:
    void finished(qint64 session);
    void samples(QVector<Sample> batch);
    void status(qint64 session, QString message, bool fatal);

private:
    QHash<qint64, ProbeSession *> sessions_;
};
