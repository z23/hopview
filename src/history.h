#pragma once

#include "types.h"
#include <QList>
#include <QSet>
#include <optional>
#include <QVariantMap>

struct sqlite3;
class RouteEvents;

// Each worker owns its connection. All timestamps are Unix milliseconds.
class History {
  public:
    explicit History(QString path);
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    bool open();
    QString error() const;
    qint64 createSession(QString target, int family, int intervalMs, QString protocol = QStringLiteral("auto"),
                       int port = 0, QString interfaceName = {});
    QVariantList sessions();
    bool setTabClosed(qint64 session, bool closed);
    bool setInterval(qint64 session, int intervalMs);
    bool addNote(qint64 session, qint64 timeMs, QString text);
    bool append(const QVector<Sample> &samples, QList<qint64> *newEvents = nullptr);
    QVariantMap monitoringSettings();
    bool saveBackground(bool enabled, const QSet<qint64> &active);
    bool setAlertRule(qint64 id, double latency, double loss, int seconds);
    // A negative cursor returns the latest 200. Otherwise read all events newer than the cursor.
    QVariantList alertEvents(qint64 afterId);
    bool resetAlertWindow(qint64 id);
    bool routePage(qint64 session, RouteEvents &detector, qint64 &cursor, qint64 &until,
                   QVariantList &events, bool &more);
    QVariantMap view(qint64 session, qint64 viewStart, qint64 viewEnd, qint64 focusStart, qint64 focusEnd,
                     QList<int> selectedHops, int pixels, bool timelinesOnly = false);
    bool exportIncident(qint64 session, qint64 from, qint64 through, const QString &file, bool csv);

  private:
    struct AlertState {
        qint64 id, intervalMs, durationMs, last, first, sent, received, candidateSince, incidentSince;
        double latency, loss, total;
        int candidate;
    };
    void createMonitoringTables();
    std::optional<AlertState> loadAlertState(qint64 id);
    void saveAlertState(const AlertState &s);
    void evaluateDestination(AlertState &s, qint64 timeMs, double rtt, QList<qint64> &eventIds);
    QString path_;
    QString error_;
    sqlite3 *db_ = nullptr;
};
