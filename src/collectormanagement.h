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
    QString error() const { return error_; }

    void setCertificateFingerprint(const QString &fingerprint);
    bool hasSite(const QString &site);
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
