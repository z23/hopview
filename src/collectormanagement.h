#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QVariantList>

struct sqlite3;

class CollectorManagement {
  public:
    explicit CollectorManagement(QString databasePath);
    ~CollectorManagement();
    CollectorManagement(const CollectorManagement &) = delete;
    CollectorManagement &operator=(const CollectorManagement &) = delete;

    QString createKey();
    QVariantList nodes();
    bool setTargets(const QString &site, qint64 expectedRevision, const QVariantList &targets);
    bool removeCollector(const QString &site);
    int revokeUnusedKeys();
    QString error() const { return error_; }

    void setCertificateFingerprint(const QString &fingerprint);
    void saveHealth(const QString &site, const QJsonObject &health, qint64 received);
    bool hasSite(const QString &site);
    bool hasKeyDigest(const QByteArray &digest);
    QJsonObject enroll(const QString &secret, const QString &nodeId, const QString &hostname,
                       const QStringList &reservedSites, qint64 appliedRevision, const QString &applyError,
                       qint64 nowMs);
    QJsonObject heartbeat(const QString &site, qint64 appliedRevision, const QString &applyError, qint64 nowMs);

  private:
    void open();
    QJsonObject recordStatus(const QString &site, qint64 appliedRevision, const QString &applyError, qint64 nowMs);
    QString path_, error_;
    sqlite3 *db_ = nullptr;
};
