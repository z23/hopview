#pragma once
#include "types.h"
#include <QHash>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QVariant>
#include <QUrl>
#include <memory>
#include <mutex>
#include <atomic>

struct IncidentRange;
class History;
class Probe;
class Diagnostics;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap diagnosticsSettings READ diagnosticsSettings NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool backgroundAvailable READ backgroundAvailable NOTIFY monitoringChanged)
    Q_PROPERTY(bool backgroundEnabled READ backgroundEnabled NOTIFY monitoringChanged)
    Q_PROPERTY(QVariantMap alertRules READ alertRules NOTIFY monitoringChanged)
    Q_PROPERTY(QVariantList alertEvents READ alertEvents NOTIFY alertsChanged)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantList closedSessions READ closedSessions NOTIFY sessionsChanged)
    Q_PROPERTY(qint64 sessionId READ sessionId NOTIFY selectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool running READ running NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 viewStart READ viewStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 viewEnd READ viewEnd NOTIFY viewportChanged)
    Q_PROPERTY(qint64 focusStart READ focusStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 focusEnd READ focusEnd NOTIFY viewportChanged)
    Q_PROPERTY(bool live READ live NOTIFY viewportChanged)
    Q_PROPERTY(qreal viewSeconds READ viewSeconds NOTIFY viewportChanged)
    Q_PROPERTY(int focusSeconds READ focusSeconds NOTIFY viewportChanged)
    Q_PROPERTY(QVariantMap savedBaseline READ savedBaseline NOTIFY baselineChanged)
    Q_PROPERTY(QString databasePath READ databasePath CONSTANT)
    Q_PROPERTY(bool replayMode READ replayMode CONSTANT)
    Q_PROPERTY(QStringList groupNames READ groupNames NOTIFY groupsChanged)
    Q_PROPERTY(QVariantList comparisonIds READ comparisonIds NOTIFY viewportChanged)
    Q_PROPERTY(QVariantList selectedHops READ selectedHops NOTIFY viewportChanged)
  public:
    Q_INVOKABLE QStringList interfaceNames() const;
    explicit Backend(const QString &file, QObject *parent = nullptr);
    ~Backend() override;
    QVariantMap diagnosticsSettings() const { return m_diagnosticsSettings; }
    Q_INVOKABLE void setDiagnostics(qint64 id, QString url, QString interfaceName, int seconds);
    void enableRecorder();
    void setWindowVisible(bool visible);
    bool backgroundAvailable() const { return m_recorderAvailable; }
    bool backgroundEnabled() const { return m_background; }
    QVariantMap alertRules() const { return m_alertRules; }
    QVariantList alertEvents() const { return m_alertEvents; }
    Q_INVOKABLE void setBackgroundEnabled(bool enabled);
    Q_INVOKABLE void setAlertRule(qint64 id, double latency, double loss, int seconds);
    Q_INVOKABLE void showAlert(qint64 session, qint64 from, qint64 through);
    QVariantList sessions() const { return m_sessions; }
    QVariantList closedSessions() const { return m_closedSessions; }
    qint64 sessionId() const { return m_session; }
    QString status() const { return m_status; }
    bool running() const { return m_active.contains(m_session); }
    QVariantMap snapshot() const { return m_snapshot; }
    qint64 viewStart() const { return m_viewEnd - m_spanMs; }
    qint64 viewEnd() const { return m_viewEnd; }
    qint64 focusStart() const;
    qint64 focusEnd() const;
    bool live() const { return m_live; }
    qreal viewSeconds() const { return m_spanMs / 1000.0; }
    int focusSeconds() const { return m_focusSecs; }
    QVariantList selectedHops() const;
    QString databasePath() const { return m_historyPath; }
    bool replayMode() const { return m_replayMode; }
    QStringList groupNames() const { return m_groupNames; }
    void restoreIncident(const IncidentRange &range);
    Q_INVOKABLE void saveGroup(QString name);
    Q_INVOKABLE void openGroup(QString name);
    Q_INVOKABLE void deleteGroup(QString name);
    Q_INVOKABLE void addIncident(QUrl file, QString site, qint64 correctionMs);
    void setReplayMode() { m_replayMode = true; }
    QVariantMap savedBaseline() const;
    Q_INVOKABLE void markBaseline();
    Q_INVOKABLE void compareFocus(bool previous);
    Q_INVOKABLE void cancelBaseline() { ++m_baselineVersion; }
    QVariantList comparisonIds() const;
    Q_INVOKABLE void toggleComparison(qint64 id);
    Q_INVOKABLE void exportIncident(qint64 session, qint64 from, qint64 through, QUrl file, bool csv,
                                    QVariantList peers = {}, QVariantList hops = {0});
    Q_INVOKABLE void openReplay(QUrl file);

    Q_INVOKABLE void startTrace(QString target, int family, int intervalMs, QString protocol = QStringLiteral("auto"),
                       int port = 0, QString interfaceName = {}, bool reuseRunning = false);
    Q_INVOKABLE void selectSession(qint64 id);
    Q_INVOKABLE void closeSession(qint64 id);
    Q_INVOKABLE void reopenSession(qint64 id);
    Q_INVOKABLE void zoomToRange(qint64 a, qint64 b);
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void setIntervalMs(int intervalMs);
    Q_INVOKABLE void requestRouteHistory();
    Q_INVOKABLE void cancelRouteHistory();
    Q_INVOKABLE void saveNote(qint64 stamp, const QString &text);
    Q_INVOKABLE void toggleHop(int hop);
    Q_INVOKABLE void setViewSeconds(int seconds);
    Q_INVOKABLE void setFocusSeconds(int seconds);
    Q_INVOKABLE void focusAt(qint64 stamp);
    Q_INVOKABLE void panBy(qint64 deltaMs);
    Q_INVOKABLE void goLive();
    Q_INVOKABLE void setPixels(int pixels);
  signals:
    void diagnosticsChanged();
    void diagnosticSettingsSaved(qint64 id, QString error);
    void baselineChanged();
    void comparisonReady(QVariantList rows, qint64 from, qint64 through, qint64 incidentFrom, qint64 incidentEnd, QString error);
    void monitoringChanged();
    void alertsChanged();
    void destinationAlert(QVariantMap event);
    void sessionsChanged();
    void selectionChanged();
    void snapshotChanged();
    void routeHistoryPage(QVariantList events, bool finished, QString error);
    void statusChanged();
    void viewportChanged();
    void groupsChanged();
    void incidentAdded(bool ok, QString message);
    void incidentExported(bool ok, QString message);

  private:
    void setStatus(QString message);
    void scheduleView(bool invalidate = true);
    void fetchView();
    void runSession(qint64 id);
    void changeTabVisibility(qint64 id, bool closed, qint64 from = 0, qint64 through = 0);
    void syncDiagnostics(qint64 id);
    QVariantMap m_diagnosticsSettings;
    QThread m_diagnosticThread;
    Diagnostics *m_diagnostics = nullptr;
    void persistRecordingSet();
    QVariantMap m_alertRules;
    QVariantList m_alertEvents, m_resumeSessions;
    bool m_background = false, m_recorderAvailable = false, m_windowVisible = true;
    void setActive(qint64 id, bool active);
    void acceptSamples(QVector<Sample> batch);
    void flushSamples();
    void recordingError(QString message);
    struct RouteScan;
    void readRoutePage(std::shared_ptr<RouteScan> scan);
    quint64 m_routeRequest = 0;
    void resolveNames();
    void refreshNames();
    QStringList m_groupNames;
    void displayComparison(const QList<qint64> &members, const QList<int> &hops);
    QVariantMap sessionInfo(qint64 id) const;
    qint64 latest() const;
    qint64 earliest() const;
    QHash<qint64, QVariantMap> m_baselines;
    std::atomic<quint64> m_baselineVersion{0};
    QThreadPool m_baselinePool;
    const QString m_historyPath;
    QThreadPool m_exports;
    QThread m_collectorThread, m_writerThread, m_readThread;
    QObject *m_writerContext = nullptr, *m_readContext = nullptr;
    Probe *m_probe = nullptr;
    std::unique_ptr<History> m_writer, m_reader;
    QVariantList m_sessions, m_closedSessions;
    QVariantMap m_snapshot;
    QString m_status;
    // Accessed on the history reader thread. Selecting a new focus point within
    // the same timeline only requires recalculating the table.
    struct ChartCache {
        qint64 id = 0, start = 0, end = 0;
        int pixels = 0;
        quint64 revision = 0;
        QList<int> hops;
        QVariantList charts;
        double timeMax = 0;
    };
    ChartCache m_chartCache;
    QHash<qint64, ChartCache> m_comparisonCache;
    QHash<qint64, quint64> m_revisions;
    QHash<qint64, QString> m_messages;
    QSet<qint64> m_active;
    QList<QVariantList> m_pendingStarts;
    QSet<QString> m_lookups;
    QHash<QString, QString> m_names;
    struct SessionView {
        QList<int> hops{0};
        qint64 spanMs = 300000;
        int focusSeconds = 60;
        bool live = true;
        qint64 end = 0, center = 0;
        QList<qint64> comparisons;
    };
    QHash<qint64, SessionView> m_views;
    QList<int> m_hops{0};
    QList<qint64> m_comparisons;
    bool m_replayMode = false;
    std::mutex m_batchMutex;
    QVector<Sample> m_waitingSamples;
    bool m_flushScheduled = false, m_queueOverflow = false;
    void pollReceiver();
    QVariantMap labelAlert(QVariantMap event) const;
    void publishAlerts(const QVariantList &events);
    bool m_remoteBusy = false;
    qint64 m_receiverVersion = -1; // Owned by the history reader thread.
    QTimer m_refresh, m_coalesce, m_remotePoll;
    qint64 m_session = 0, m_viewEnd = 0, m_focusCenter = 0;
    qint64 m_spanMs = 300000;
    int m_focusSecs = 60, m_pixels = 1000;
    quint64 m_version = 0;
    bool m_live = true, m_busy = false, m_pending = false, m_ready = false, m_dirty = false;
};
