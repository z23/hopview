#pragma once
#include "types.h"
#include <QHash>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <memory>
#include <mutex>

class History;
class Probe;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(qint64 sessionId READ sessionId NOTIFY selectionChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool running READ running NOTIFY sessionsChanged)
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 viewStart READ viewStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 viewEnd READ viewEnd NOTIFY viewportChanged)
    Q_PROPERTY(qint64 focusStart READ focusStart NOTIFY viewportChanged)
    Q_PROPERTY(qint64 focusEnd READ focusEnd NOTIFY viewportChanged)
    Q_PROPERTY(bool live READ live NOTIFY viewportChanged)
    Q_PROPERTY(int viewSeconds READ viewSeconds NOTIFY viewportChanged)
    Q_PROPERTY(int focusSeconds READ focusSeconds NOTIFY viewportChanged)
    Q_PROPERTY(QVariantList selectedHops READ selectedHops NOTIFY viewportChanged)
  public:
    explicit Backend(const QString &file, QObject *parent = nullptr);
    ~Backend() override;
    QVariantList sessions() const { return m_sessions; }
    qint64 sessionId() const { return m_session; }
    QString status() const { return m_status; }
    bool running() const { return m_active.contains(m_session); }
    QVariantMap snapshot() const { return m_snapshot; }
    qint64 viewStart() const { return m_viewEnd - qint64(m_viewSecs) * 1000; }
    qint64 viewEnd() const { return m_viewEnd; }
    qint64 focusStart() const;
    qint64 focusEnd() const;
    bool live() const { return m_live; }
    int viewSeconds() const { return m_viewSecs; }
    int focusSeconds() const { return m_focusSecs; }
    QVariantList selectedHops() const;
    Q_INVOKABLE void startTrace(QString target, int family, int intervalMs);
    Q_INVOKABLE void selectSession(qint64 id);
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void setIntervalMs(int intervalMs);
    Q_INVOKABLE void saveNote(qint64 stamp, const QString &text);
    Q_INVOKABLE void toggleHop(int hop);
    Q_INVOKABLE void setViewSeconds(int seconds);
    Q_INVOKABLE void setFocusSeconds(int seconds);
    Q_INVOKABLE void focusAt(qint64 stamp);
    Q_INVOKABLE void panBy(qint64 deltaMs);
    Q_INVOKABLE void goLive();
    Q_INVOKABLE void setPixels(int pixels);
  signals:
    void sessionsChanged();
    void selectionChanged();
    void snapshotChanged();
    void statusChanged();
    void viewportChanged();

  private:
    void setStatus(QString message);
    void scheduleView(bool invalidate = true);
    void fetchView();
    void runSession(qint64 id);
    void setActive(qint64 id, bool active);
    void acceptSamples(QVector<Sample> batch);
    void flushSamples();
    void recordingError(QString message);
    void resolveNames();
    void refreshNames();
    QVariantMap sessionInfo(qint64 id) const;
    qint64 latest() const;
    qint64 earliest() const;
    QThread m_collectorThread, m_writerThread, m_readThread;
    QObject *m_writerContext = nullptr, *m_readContext = nullptr;
    Probe *m_probe = nullptr;
    std::unique_ptr<History> m_writer, m_reader;
    QVariantList m_sessions;
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
    QHash<qint64, quint64> m_revisions;
    QHash<qint64, QString> m_messages;
    QSet<qint64> m_active;
    QSet<QString> m_lookups;
    QHash<QString, QString> m_names;
    struct SessionView {
        QList<int> hops{0};
        int viewSeconds = 300, focusSeconds = 60;
        bool live = true;
        qint64 end = 0, center = 0;
    };
    QHash<qint64, SessionView> m_views;
    QList<int> m_hops{0};
    std::mutex m_batchMutex;
    QVector<Sample> m_waitingSamples;
    bool m_flushScheduled = false, m_queueOverflow = false;
    QTimer m_refresh, m_coalesce;
    qint64 m_session = 0, m_viewEnd = 0, m_focusCenter = 0;
    int m_viewSecs = 300, m_focusSecs = 60, m_pixels = 1000;
    quint64 m_version = 0;
    bool m_live = true, m_busy = false, m_pending = false, m_ready = false, m_dirty = false;
};
