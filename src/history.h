#pragma once

#include "types.h"
#include <QList>
#include <QSet>
#include <optional>
#include <functional>
#include <QVariantMap>

struct sqlite3;
class RouteEvents;

// Each worker owns its connection. Timestamps count milliseconds since 1970-01-01 00:00 UTC.
class History {
  public:
    explicit History(QString path);
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    bool open();
    bool openReadOnly();
    QString error() const;
    qint64 createSession(QString target, int family, int intervalMs, QString protocol = QStringLiteral("auto"),
                       int port = 0, QString interfaceName = {}, int payloadBytes = 16, int dscp = 0, bool dontFragment = false);
    qint64 dataVersion();
    QVariantList sessions(bool remoteOnly = false, qint64 onlyId = 0);
    qint64 receiverSession(const QString &location, const QString &stream, const QString &target,
                           int family, int intervalMs, const QString &protocol, int port, const QString &interfaceName, int payloadBytes = 16, int dscp = 0, bool dontFragment = false);
    bool setSessionOrigin(qint64 session, const QString &location, const QString &resolvedAddress,
                          qint64 clockOffsetMs = 0, qint64 clockErrorMs = -1, qint64 lastReceivedMs = 0,
                          qint64 sampleThroughMs = -1);
    qint64 lastDestinationSampleAt(qint64 id, qint64 through);
    QVariantMap destinationPeriods(qint64 id, qint64 from, qint64 through, qint64 baseFrom = -1, qint64 baseEnd = -1);
    QVariantMap retainedDestination(qint64 id, qint64 from, qint64 through);
    QVariantList comparePeriods(qint64 id, qint64 baselineFrom, qint64 baselineThrough, qint64 incidentFrom,
                                qint64 incidentThrough, const std::function<bool()> &cancel);
    QVariantList namedBaselines();
    bool storeBaseline(qint64 id, QString name, qint64 from, qint64 through, double p95);
    double destinationP95(qint64 id, qint64 from, qint64 through);
    bool setMeasurement(qint64 id, qint64 stamp, const QVariantMap &settings);
    QVariantMap focusedMeasurement(qint64 id, qint64 from, qint64 through);
    bool setTabClosed(qint64 session, bool closed);
    bool setInterval(qint64 session, int intervalMs);
    bool addNote(qint64 session, qint64 timeMs, QString text);
    bool append(const QVector<Sample> &samples, QList<qint64> *newEvents = nullptr);
    QVariantMap diagnosticsSettings();
    bool saveDiagnostics(qint64 id, const QString &url, const QString &device, int seconds);
    bool appendDiagnostic(qint64 id, const QVariantMap &record);
    bool eachDiagnostic(qint64 id, qint64 from, qint64 through,
                        const std::function<void(const QVariantMap &)> &visit);
    QVariantList diagnosticTracks(qint64 id, qint64 from, qint64 through, int pixels);
    QVariantMap monitoringSettings();
    bool saveBackground(bool enabled, const QSet<qint64> &active);
    bool setAlertRule(qint64 id, double latency, double loss, int seconds);
    // A negative cursor returns the latest 200. Otherwise read all events newer than the cursor.
    QVariantList alertEvents(qint64 afterId);
    bool resetAlertWindow(qint64 id);
    bool setRetention(int rawDays,int summaryDays);
    bool maintainStorage(qint64 now);
    bool releasePin(qint64 id, qint64 from, qint64 through);
    bool pinInterval(QList<qint64> ids,qint64 from,qint64 through);
    bool onlineBackup(const QString &destination);
    QVariantMap storageStatus();
    QString retainedResolution(qint64 id,qint64 from,qint64 through);
    bool importInventory(const QString &path);
    QVariantMap networkIdentity(qint64 id, const QString &address);
    bool captureIdentity(qint64 id, const QString &address, const QVariantMap &info);
    QVariantList responders(qint64 id, int hop, qint64 from, qint64 through);
    QVariantList timelineEvents(qint64 session, qint64 from, qint64 through);
    bool advanceRouteIndex(qint64 session,bool &more);
    bool indexedRoutes(qint64 session, qint64 &cursor, QVariantList &events, bool &more);
    bool routePage(qint64 session, RouteEvents &detector, qint64 &cursor, qint64 &until,
                   QVariantList &events, bool &more);
    QVariantMap view(qint64 session, qint64 viewStart, qint64 viewEnd, qint64 focusStart, qint64 focusEnd,
                     QList<int> selectedHops, int pixels, bool timelinesOnly = false);
    bool exportIncident(qint64 session, qint64 from, qint64 through, const QString &file, bool csv,
                        QList<qint64> comparisons = {}, QList<int> pinnedHops = {0});
    bool markImported(qint64 session, qint64 correction);
    bool adoptIncident(const QString &database, QList<qint64> &sessions, qint64 from, qint64 through);
    QStringList groupNames();
    bool saveGroup(QString name, const QList<qint64> &sessions, const QList<int> &hops, QVariantMap view = {});
    bool openGroup(const QString &name, QList<qint64> &sessions, QList<int> &hops, QVariantMap *view = nullptr);
    bool deleteGroup(const QString &name);

  private:
    struct AlertState {
        qint64 id, intervalMs, durationMs, last, first, sent, received, candidateSince, incidentSince;
        double latency, loss, total;
        int candidate;
    };
    void createDiagnosticTables();
    void createMonitoringTables();
    std::optional<AlertState> loadAlertState(qint64 id);
    void saveAlertState(const AlertState &s);
    void evaluateDestination(AlertState &s, qint64 timeMs, double rtt, QList<qint64> &eventIds);
    bool updateRouteIndex(qint64 session, qint64 firstNew);
    QString path_;
    QString error_;
    sqlite3 *db_ = nullptr;
};
