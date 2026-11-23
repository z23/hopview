#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <memory>

struct InterfaceReading {
    QString name, ap;
    qint64 tick = 0;
    int index = 0, carrier = -1;
    quint64 rx = 0, tx = 0, rxDrops = 0, txDrops = 0, rxErrors = 0, txErrors = 0;
    bool readable = false;
};

QVariantMap interfaceSample(const InterfaceReading &current, const InterfaceReading *previous, qint64 time);
QVariantMap measureService(const QString &url, int family, const QString &bindInterface, int seconds,
                           const std::atomic_bool &cancelled, const QByteArray &caFile = {});

class Diagnostics : public QObject {
    Q_OBJECT
  public:
    Diagnostics();
    void configure(qint64 id, const QVariantMap &settings, int family, const QString &bindInterface);
    void shutdown();
  signals:
    void observation(qint64 session, QVariantMap record);

  private:
    struct Watch {
        QString interface, url, bindInterface;
        int family = 0, seconds = 30;
        qint64 nextCheck = 0, nextInterface = 0;
        InterfaceReading previous;
        bool busy = false;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    void poll();
    QHash<qint64, Watch> watches_;
    QElapsedTimer clock_;
    QTimer *timer_;
    QThreadPool pool_;
};
