#pragma once

#include "receiverendpoint.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class LocalReceiver : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString database READ database WRITE setDatabase NOTIFY databaseChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
  public:
    explicit LocalReceiver(QObject *parent = nullptr);
    ~LocalReceiver() override;
    QString database() const { return database_; }
    void setDatabase(const QString &path);
    bool running() const { return ready_; }
    bool busy() const { return busy_; }
    QString message() const { return message_; }
    Q_INVOKABLE void start(const QString &address);
    Q_INVOKABLE void stop();

  signals:
    void databaseChanged();
    void stateChanged();

  private:
    void launchServer();
    void report(bool busy, bool ready, const QString &message);
    QProcess certificate_, server_;
    QTimer killTimer_;
    std::unique_ptr<QTemporaryDir> staging_;
    telemetry::ReceiverEndpoint endpoint_;
    QString database_, directory_, message_, lastLine_;
    bool ready_ = false, busy_ = false, stopping_ = false;
};
