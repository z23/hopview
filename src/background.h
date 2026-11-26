#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLockFile>
#include <QVariant>
#include <functional>
#include <memory>

class QSystemTrayIcon;
class BackgroundInstance : public QObject {
    Q_OBJECT
  public:
    enum Result { Failed, Forwarded, Owner };
    Result acquire(const QString &database, const QJsonObject &request, QString &error);
    void handleRequests(std::function<void(const QJsonObject &)> fn);
    void notify(const QVariantMap &event);
    static void enableGracefulExit();
  signals:
    void openEvent(QVariantMap event);
#ifdef Q_OS_LINUX
  private slots:
    void notificationAction(uint id, const QString &action);
    void notificationClosed(uint id, uint reason);
#endif
  private:
    void acceptClients();
    std::unique_ptr<QLockFile> lock_;
    QLocalServer server_;  // Close the socket before releasing ownership of its name.
    std::function<void(const QJsonObject &)> handler_;
#ifdef Q_OS_LINUX
    QHash<uint, QVariantMap> notices_;
#else
    QSystemTrayIcon *tray_ = nullptr;
#endif
};
