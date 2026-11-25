#include "probeoptions.h"
#include <QNetworkInterface>
#include "backend.h"
#include "collectormanagement.h"
#include "history.h"
#include "routeevents.h"
#include "diagnostics.h"
#include "probe.h"
#include <QDateTime>
#include <QHostInfo>
#include <QHostAddress>
#include <QProcess>
#include <QCoreApplication>
#include <algorithm>
#include <utility>

namespace {
QObject *contextOn(QThread &thread) {
    auto *context = new QObject;
    context->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, context, &QObject::deleteLater);
    thread.start();
    return context;
}
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
} // namespace

Backend::Backend(const QString &file, QObject *parent) : QObject(parent), m_historyPath(file) {
    m_exports.setMaxThreadCount(1);
    m_baselinePool.setMaxThreadCount(1);
    m_writerContext = contextOn(m_writerThread);
    m_readContext = contextOn(m_readThread);
    m_probe = new Probe;
    m_probe->moveToThread(&m_collectorThread);
    connect(&m_collectorThread, &QThread::finished, m_probe, &QObject::deleteLater);
    m_collectorThread.start();
    QMetaObject::invokeMethod(
        m_writerContext,
        [&] {
            m_writer = std::make_unique<History>(file);
            m_ready = m_writer->open();
            if (m_ready) {
                m_sessions = m_writer->sessions();
                m_diagnosticsSettings = m_writer->diagnosticsSettings();
                for (const auto &value : m_writer->namedBaselines()) {
                    const auto mark = value.toMap();
                    const auto session = mark["session"].toLongLong();
                    m_namedBaselines[session][mark["name"].toString()] = mark;
                    if (mark["active"].toBool())
                        m_baselines[session] = mark;
                }
                m_groupNames = m_writer->groupNames();
                const auto settings = m_writer->monitoringSettings();
                m_resumeSessions = settings["active"].toList();
                m_background = settings["background"].toBool();
                m_alertRules = settings["rules"].toMap();
                m_alertEvents = m_writer->alertEvents(-1);
            } else
                m_status = m_writer->error();
        },
        Qt::BlockingQueuedConnection);
    if (m_ready) {
        QMetaObject::invokeMethod(
            m_readContext,
            [&] {
                m_reader = std::make_unique<History>(file);
                m_ready = m_reader->openReadOnly();
                if (!m_ready)
                    m_status = m_reader->error();
            },
            Qt::BlockingQueuedConnection);
    }
    const auto recordings = std::exchange(m_sessions, {});
    for (const auto &entry : recordings) {
        auto info = entry.toMap();
        info["running"] = false;
        (info.take("closed").toBool() ? m_closedSessions : m_sessions).append(info);
    }
    for (auto &event : m_alertEvents) event = labelAlert(event.toMap());
    connect(m_probe,&Probe::measurement,this,[this](qint64 id,qint64 stamp,const QVariantMap &settings) {
        for (auto &entry : m_sessions) {
            auto info = entry.toMap();
            if (info["id"].toLongLong() != id) continue;
            for (auto it=settings.cbegin();it!=settings.cend();++it) info[it.key()] = it.value();
            entry = info;
            emit sessionsChanged();
            break;
        }
        QMetaObject::invokeMethod(m_writerContext,[this,id,stamp,settings] {
            if (!m_writer->setMeasurement(id,stamp,settings)) {
                auto error=m_writer->error();
                QMetaObject::invokeMethod(this,[this,error]{ setStatus(error); });
            }
        });
    });
    connect(m_probe,&Probe::replyReceived,this,[this](qint64 id,qint64 stamp,int hop,const QString &address,double rtt) {
        if (id != m_session || !m_active.contains(id) || !m_windowVisible || !m_live || stamp < m_provisionalStamp) return;
        if (stamp != m_provisionalStamp) m_replyLabels.clear();
        m_provisionalStamp = stamp;
        m_replyLabels[hop] = tr("%1 %2 · %3 ms").arg(hop ? tr("Hop %1").arg(hop) : tr("Target"), address, QString::number(rtt,'f',1));
        const auto time = QDateTime::fromMSecsSinceEpoch(stamp).toUTC().toString("HH:mm:ss.zzz");
        m_provisionalReplies = time + " UTC    " + m_replyLabels.values().join(QStringLiteral("    "));
        emit provisionalChanged();
    });
    connect(m_probe, &Probe::status, this, [this](qint64 id, QString message, bool fatal) {
        if (!m_active.contains(id))
            return;
        m_messages[id] = message;
        if (fatal)
            setActive(id, false);
        if (id == m_session)
            setStatus(message);
    });
    connect(m_probe, &Probe::samples, m_probe,
            [this](QVector<Sample> batch) { acceptSamples(std::move(batch)); });
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(8);
    connect(&m_coalesce, &QTimer::timeout, this, &Backend::fetchView);
    m_refresh.setInterval(750);
    connect(&m_refresh, &QTimer::timeout, this, [this] {
        if (!m_windowVisible || !m_session || (!m_dirty && !running()))
            return;
        m_dirty = false;
        if (m_live) {
            m_viewEnd = latest();
            emit viewportChanged();
            scheduleView(false);
        }
    });
    m_refresh.start();
    auto *maintenance = new QTimer(this);
    connect(maintenance,&QTimer::timeout,this,[this] {
        if (m_ready && !m_replayMode)
            m_exports.start([this] {
                History store(m_historyPath);
                if (!store.maintainStorage(now())) {
                    const auto error = store.error();
                    QMetaObject::invokeMethod(this,[this,error] { setStatus(error); });
                } else {
                    QMetaObject::invokeMethod(this,[this] { ++m_storageRevision; });
                }
            });
    });
    maintenance->start(3600000);
    m_remotePoll.setInterval(2000);
    connect(&m_remotePoll,&QTimer::timeout,this,&Backend::pollReceiver);
    m_remotePoll.start();
    if (!m_sessions.isEmpty())
        selectSession(m_sessions.first().toMap().value("id").toLongLong());
}

Backend::~Backend() {
    m_closing.store(true);
    m_remotePoll.stop();
    m_refresh.stop();
    m_coalesce.stop();
    cancelBaseline();
    cancelFleet();
    ++m_summaryRequest;
    m_baselinePool.waitForDone();
    m_exports.waitForDone();
    if (m_diagnostics) {
        QMetaObject::invokeMethod(m_diagnostics, &Diagnostics::shutdown, Qt::BlockingQueuedConnection);
        m_diagnosticThread.quit();
        m_diagnosticThread.wait();
    }
    QMetaObject::invokeMethod(m_probe, [this] { m_probe->shutdown(); }, Qt::BlockingQueuedConnection);
    m_collectorThread.quit();
    m_collectorThread.wait();
    // Flush pending samples and drain writer-to-reader posts before closing reader,
    // then drain any reader-to-writer posts (such as baseline persistence) before closing writer.
    QMetaObject::invokeMethod(m_writerContext, [this] { flushSamples(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(m_readContext, [this] { m_reader.reset(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(
        m_writerContext,
        [this] {
            m_writer.reset();
            m_management.reset();
        },
        Qt::BlockingQueuedConnection);
    m_writerThread.quit();
    m_readThread.quit();
    m_writerThread.wait();
    m_readThread.wait();
}

// Producer-side buffering combines independent traces into one WAL transaction.
// The only cross-thread lock protects the short append/swap, never disk I/O.
void Backend::acceptSamples(QVector<Sample> batch) {
    if (batch.isEmpty())
        return;
    std::lock_guard lock(m_batchMutex);
    constexpr qsizetype maxBuffered = 65536;
    if (m_waitingSamples.size() + batch.size() > maxBuffered) {
        if (!m_queueOverflow) {
            m_queueOverflow = true;
            QMetaObject::invokeMethod(this, [this] {
                recordingError(tr("Storage fell behind; collection paused. Some samples could not be saved."));
            });
        }
        return;
    }
    m_waitingSamples.append(std::move(batch));
    if (m_flushScheduled)
        return;
    m_flushScheduled = true;
    QTimer::singleShot(75, m_writerContext, [this] { flushSamples(); });
}

void Backend::recordingError(QString message) {
    const auto active = m_active;
    for (auto id : active) {
        setActive(id, false);
        m_messages[id] = message;
        QMetaObject::invokeMethod(m_probe, [this, id] { m_probe->stop(id); });
    }
    setStatus(message);
}

void Backend::flushSamples() {
    QVector<Sample> batch;
    {
        std::lock_guard lock(m_batchMutex);
        batch.swap(m_waitingSamples);
        m_flushScheduled = false;
        m_queueOverflow = false;
    }
    if (batch.isEmpty() || !m_writer)
        return;
    QList<qint64> eventIds;
    if (!m_writer->append(batch, &eventIds)) {
        const QString error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, error] {
            recordingError(tr("History write failed; collection paused. %1").arg(error));
        });
        return;
    }
    if (!eventIds.isEmpty()) {
        QMetaObject::invokeMethod(this,[this] {
            const auto last = m_alertEvents.isEmpty() ? 0LL : m_alertEvents.first().toMap()["id"].toLongLong();
            QMetaObject::invokeMethod(m_readContext,[this,last] {
                const auto events = m_reader->alertEvents(last);
                QMetaObject::invokeMethod(this,[this,events] { publishAlerts(events); });
            });
        });
    }
    QHash<qint64, qint64> ends;
    for (const auto &s : batch)
        ends[s.session] = std::max(ends.value(s.session), s.timeMs);
    QMetaObject::invokeMethod(this, [this, ends = std::move(ends)]() mutable {
        const bool selectedUpdated = ends.contains(m_session) || std::any_of(m_comparisons.cbegin(), m_comparisons.cend(), [&](qint64 id) { return ends.contains(id); });
        // Most batches belong entirely to open tabs. A final batch may arrive
        // after a tab closes, while its recording is still being flushed.
        for (auto *list : {&m_sessions, &m_closedSessions}) {
            if (ends.isEmpty())
                break;
            for (auto &entry : *list) {
                auto info = entry.toMap();
                const auto id = info.value("id").toLongLong();
                auto stamp = ends.find(id);
                if (stamp == ends.end())
                    continue;
                ++m_revisions[id];
                info["endMs"] = std::max(info.value("endMs").toLongLong(), stamp.value());
                entry = info;
                ends.erase(stamp);
            }
        }
        if (selectedUpdated) {
            m_dirty = true;
            if (m_snapshot.isEmpty() && m_windowVisible)
                scheduleView(false);
        }
    });
}

QVariantMap Backend::sessionInfo(qint64 id) const {
    for (const auto &entry : m_sessions) {
        auto info = entry.toMap();
        if (info.value("id").toLongLong() == id)
            return info;
    }
    return {};
}

qint64 Backend::earliest() const { return sessionInfo(m_session).value("startMs").toLongLong(); }
qint64 Backend::latest() const {
    auto end = sessionInfo(m_session).value("endMs").toLongLong();
    if (running()) return now();
    for (auto peer : m_comparisons) {
        if (m_active.contains(peer)) return now();
        end = std::max(end, sessionInfo(peer).value("endMs").toLongLong());
    }
    return end;
}
qint64 Backend::focusEnd() const {
    if (m_focusSecs < 0)
        return std::max(m_viewEnd, sessionInfo(m_session).value("endMs").toLongLong());
    if (!m_focusCenter || m_focusSecs == 0)
        return m_viewEnd;
    const qint64 duration = qint64(m_focusSecs) * 1000;
    const qint64 right = std::max(m_viewEnd, sessionInfo(m_session).value("endMs").toLongLong());
    return std::clamp(m_focusCenter + duration / 2, std::min(right, earliest() + duration), right);
}
qint64 Backend::focusStart() const {
    if (m_focusSecs < 0)
        return earliest();
    return m_focusSecs ? focusEnd() - qint64(m_focusSecs) * 1000 : viewStart();
}
QVariantList Backend::selectedHops() const {
    QVariantList result;
    for (int hop : m_hops)
        result.append(hop);
    return result;
}
QVariantList Backend::comparisonIds() const {
    QVariantList ids;
    for (auto id : m_comparisons)
        ids.append(id);
    return ids;
}

void Backend::toggleComparison(qint64 id) {
    if (!m_session || id == m_session || sessionInfo(id).isEmpty())
        return;
    if (!m_comparisons.removeOne(id))
        m_comparisons.append(id);
    if (m_live)
        m_viewEnd = latest();
    emit viewportChanged();
    scheduleView();
}

void Backend::exportIncident(qint64 id, qint64 from, qint64 through, QUrl file, bool csv, QVariantList peers, QVariantList pinned) {
    if (!m_ready || !id || !file.isLocalFile()) {
        emit incidentExported(false, tr("Choose a local output file."));
        return;
    }
    const auto path = file.toLocalFile();
    QList<qint64> members;
    for (const auto &peer : peers) members.append(peer.toLongLong());
    QList<int> hops;
    for (const auto &hop : pinned) hops.append(hop.toInt());
    m_exports.start([this, id, from, through, path, csv, members, hops] {
        History reader(m_historyPath);
        const bool ok = reader.exportIncident(id, from, through, path, csv, members, hops);
        const auto message = ok ? tr("Saved incident to %1").arg(path) : reader.error();
        QMetaObject::invokeMethod(this, [this, ok, message] { emit incidentExported(ok, message); });
    });
}

void Backend::openReplay(QUrl url) {
    if (!url.isLocalFile()) {
        setStatus(tr("Choose a local incident recording."));
        return;
    }
    // An independent window can outlive its launcher. Surface invalid files in the opening window.
    auto *viewer = new QProcess;
    connect(viewer, &QProcess::finished, viewer, &QObject::deleteLater);
    connect(viewer, &QProcess::finished, this, [this, viewer](int code, QProcess::ExitStatus) {
        if (code)
            setStatus(tr("Could not open incident: %1")
                          .arg(QString::fromUtf8(viewer->readAllStandardError()).trimmed().right(500)));
    });
    connect(viewer, &QProcess::errorOccurred, viewer, [viewer](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            viewer->deleteLater();
    });
    connect(viewer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            setStatus(tr("Could not launch the incident viewer."));
    });
    viewer->start(QCoreApplication::applicationFilePath(), {"--replay", url.toLocalFile()});
}

void Backend::setStatus(QString message) {
    if (message == m_status)
        return;
    m_status = std::move(message);
    emit statusChanged();
}

QStringList Backend::interfaceNames() const {
    QStringList names;
    for (const auto &iface : QNetworkInterface::allInterfaces())
        if (iface.flags().testFlag(QNetworkInterface::IsUp)) names.append(iface.name());
    names.sort();
    return names;
}

void Backend::startTrace(QString target, int family, int intervalMs, QString protocol, int port, QString interfaceName, bool reuseRunning, int payloadBytes, int dscp, bool dontFragment) {
    if (m_replayMode) return;
    const ProbeOptions options{protocol, port, interfaceName,payloadBytes,dscp,dontFragment};
    if (!options.valid()) {
        setStatus(tr("Check protocol, port, payload size and DSCP. TCP uses the system SYN size."));
        return;
    }
    port = options.destinationPort();
    target = target.trimmed();
    if (!m_ready)
        return;
    if (target.isEmpty() || target.size() > 255 || (family != 4 && family != 6 && family != 0) ||
        intervalMs < 250 || intervalMs > 60000) {
        setStatus(tr("Enter a hostname or address and an interval between 0.25 and 60 seconds."));
        return;
    }
    const QVariantList key{target, family, intervalMs, protocol, port, interfaceName,payloadBytes,dscp,dontFragment};
    if (reuseRunning) {
        if (m_pendingStarts.contains(key))
            return;
        for (const auto &entry : m_sessions) {
            const auto trace = entry.toMap();
            if (trace["running"].toBool() && trace["target"] == target && trace["family"].toInt() == family &&
                trace["intervalMs"].toInt() == intervalMs && trace["protocol"] == protocol &&
                trace["port"].toInt() == port && trace["interfaceName"] == interfaceName && trace["payloadBytes"].toInt() == payloadBytes && trace["dscp"].toInt()==dscp && trace["dontFragment"].toBool()==dontFragment) {
                selectSession(trace["id"].toLongLong());
                return;
            }
        }
    }
    m_pendingStarts.append(key);
    QMetaObject::invokeMethod(m_writerContext, [this, target, family, intervalMs, protocol, port, interfaceName, key,payloadBytes,dscp,dontFragment] {
        const auto id = m_writer->createSession(target, family, intervalMs, protocol, port, interfaceName,payloadBytes,dscp,dontFragment);
        const auto error = m_writer->error();
        const auto stamp = now();
        QMetaObject::invokeMethod(this, [this, id, target, family, intervalMs, error, stamp, protocol, port, interfaceName, key,payloadBytes,dscp,dontFragment] {
            m_pendingStarts.removeOne(key);
            if (id <= 0) {
                setStatus(error);
                return;
            }
            m_sessions.prepend(QVariantMap{{"id", id},
                                           {"target", target},
                                           {"family", family},
                                           {"protocol", protocol}, {"port", port}, {"interfaceName", interfaceName}, {"payloadBytes",payloadBytes},{"dscp",dscp},{"dontFragment",dontFragment},
                                           {"intervalMs", intervalMs},
                                           {"startMs", stamp},
                                           {"endMs", stamp},
                                           {"running", false}});
            selectSession(id);
            runSession(id);
            matchPair(id);
        });
    });
}
void Backend::setActive(qint64 id, bool active) {
    if (active)
        m_active.insert(id);
    else
        m_active.remove(id);
    for (auto &entry : m_sessions) {
        auto info = entry.toMap();
        if (info.value("id").toLongLong() != id)
            continue;
        info["running"] = active;
        entry = info;
        break;
    }
    syncDiagnostics(id);
    persistRecordingSet();
    emit sessionsChanged();
}
void Backend::runSession(qint64 id) {
    const auto info = sessionInfo(id);
    if (m_replayMode || info.value("remoteLive").toBool() || info.value("remote").toBool() || info.value("imported").toBool() || !m_ready || info.isEmpty())
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, id] {
        if (!m_writer->resetAlertWindow(id)) {
            auto error = m_writer->error();
            QMetaObject::invokeMethod(this, [this, error] { setStatus(error); });
        }
    });
    setActive(id, true);
    m_messages[id] = tr("Resolving target…");
    setStatus(m_messages[id]);
    goLive();
    QMetaObject::invokeMethod(m_probe, [this, id, info] {
        m_probe->start(id, info.value("target").toString(), info.value("family").toInt(),
                       info.value("intervalMs").toInt(), info.value("protocol", "auto").toString(),
                       info.value("port").toInt(), info.value("interfaceName").toString(),info.value("payloadBytes",16).toInt(),info.value("dscp").toInt(),info.value("dontFragment").toBool());
    });
}
void Backend::setIntervalMs(int intervalMs) {
    if (m_replayMode || sessionInfo(m_session).value("remoteLive").toBool() || sessionInfo(m_session).value("imported").toBool() || !m_session || intervalMs < 250 || intervalMs > 60000)
        return;
    const qint64 id = m_session;
    QMetaObject::invokeMethod(m_writerContext, [this, id, intervalMs] {
        bool ok = m_writer->setInterval(id, intervalMs);
        QString error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, id, intervalMs, ok, error] {
            if (!ok) {
                setStatus(error);
                return;
            }
            for (auto &entry : m_sessions) {
                auto info = entry.toMap();
                if (info.value("id").toLongLong() != id)
                    continue;
                info["intervalMs"] = intervalMs;
                entry = info;
                if (m_active.contains(id)) {
                    QMetaObject::invokeMethod(m_probe, [this, id, info, intervalMs] {
                        m_probe->stop(id);
                        m_probe->start(id, info.value("target").toString(), info.value("family").toInt(),
                                       intervalMs, info.value("protocol", "auto").toString(),
                                       info.value("port").toInt(), info.value("interfaceName").toString(),info.value("payloadBytes",16).toInt(),info.value("dscp").toInt(),info.value("dontFragment").toBool());
                    });
                }
                emit sessionsChanged();
                break;
            }
        });
    });
}
void Backend::saveNote(qint64 stamp, const QString &text) {
    if (!m_session || stamp <= 0)
        return;
    const auto id = m_session;
    const QString note = text.trimmed().left(2048);
    QMetaObject::invokeMethod(m_writerContext, [this, id, stamp, note] {
        const bool ok = m_writer->addNote(id, stamp, note);
        const QString error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, id, ok, error] {
            if (!ok)
                setStatus(error);
            else {
                ++m_revisions[id];
                if (id == m_session || m_comparisons.contains(id)) scheduleView();
            }
        });
    });
}
void Backend::toggleRunning() {
    if (!m_session)
        return;
    const auto id = m_session;
    if (!running()) {
        runSession(id);
        return;
    }
    setActive(id, false);
    QMetaObject::invokeMethod(m_probe, [this, id] { m_probe->stop(id); });
    m_messages[id] = tr("Paused");
    setStatus(m_messages[id]);
    scheduleView();
}
void Backend::selectSession(qint64 id) {
    if (id == m_session || (id && sessionInfo(id).isEmpty()))
        return;
    if (m_session)
        m_views[m_session] = {m_hops, m_spanMs, m_focusSecs, m_live, m_viewEnd, m_focusCenter, m_comparisons};
    cancelBaseline();
    m_session = id;
    m_probe->setPreviewSession(m_windowVisible ? id : 0);
    m_provisionalStamp = -1; m_replyLabels.clear(); m_provisionalReplies.clear(); emit provisionalChanged();
    emit baselineChanged();
    const auto saved = m_views.value(id);
    m_hops = saved.hops;
    m_spanMs = saved.spanMs;
    m_focusSecs = saved.focusSeconds;
    m_live = saved.live;
    m_comparisons = saved.comparisons;
    m_viewEnd = m_live ? latest() : saved.end;
    m_focusCenter = saved.center;
    m_snapshot.clear();
    emit selectionChanged();
    emit sessionsChanged();
    emit snapshotChanged();
    const auto origin = sessionInfo(id);
    const auto label = origin.value("remoteLive").toBool() ? tr("Receiving telemetry from %1").arg(origin.value("location").toString()) : tr("Recorded session");
    setStatus(id ? m_messages.value(id, label) : QString());
    if (!id) {
        ++m_version;
        m_pending = false;
        m_coalesce.stop();
    }
    emit viewportChanged();
    scheduleView();
    ++m_indexGeneration;
    if (id && m_ready && !m_replayMode)
        warmRouteIndex(id, m_indexGeneration);
}
void Backend::closeSession(qint64 id) { changeTabVisibility(id, true); }
void Backend::reopenSession(qint64 id) { changeTabVisibility(id, false); }

void Backend::changeTabVisibility(qint64 id, bool closed, qint64 fromMs, qint64 through) {
    if (!m_ready || id <= 0)
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, id, closed, fromMs, through] {
        const bool ok = m_writer->setTabClosed(id, closed);
        const QString error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, id, closed, ok, error, fromMs, through] {
            if (!ok) {
                setStatus(error);
                return;
            }
            auto &from = closed ? m_sessions : m_closedSessions;
            auto &to = closed ? m_closedSessions : m_sessions;
            for (qsizetype i = 0; i < from.size(); ++i) {
                if (from.at(i).toMap().value("id").toLongLong() != id)
                    continue;
                auto info = from.takeAt(i).toMap();
                if (closed && m_active.remove(id)) {
                    syncDiagnostics(id);
                    persistRecordingSet();
                    info["running"] = false;
                    m_messages[id] = tr("Paused");
                    QMetaObject::invokeMethod(m_probe, [this, id] { m_probe->stop(id); });
                }
                to.prepend(info);
                if (closed) {
                    for (auto &saved : m_views)
                        saved.comparisons.removeAll(id);
                    if (m_comparisons.removeAll(id)) {
                        if (m_live) m_viewEnd = latest();
                        emit viewportChanged();
                        scheduleView();
                    }
                }
                if (!closed) {
                    selectSession(id);
                    if (through > fromMs) zoomToRange(fromMs, through);
                } else if (id == m_session) {
                    const auto next =
                        from.isEmpty() ? 0
                                       : from.at(std::min(i, from.size() - 1)).toMap().value("id").toLongLong();
                    selectSession(next);
                } else
                    emit sessionsChanged();
                return;
            }
        });
    });
}

void Backend::toggleHop(int hop) {
    if (hop < 0 || hop > 255)
        return;
    for (const auto &row : m_snapshot.value("rows").toList()) {
        if (row.toMap().value("hop").toInt() == hop && row.toMap().value("destination").toBool()) {
            hop = 0;
            break;
        }
    }
    // A destination can time out throughout the focus window.
    for (const auto &chart : m_snapshot.value("charts").toList()) {
        if (!chart.toMap().contains("comparisonId") && chart.toMap().value("destination").toBool() && chart.toMap().value("hop").toInt() == hop) {
            hop = 0;
            break;
        }
    }
    if (!m_hops.removeOne(hop))
        m_hops.append(hop);
    emit viewportChanged();
    scheduleView();
}
void Backend::setViewSeconds(int seconds) {
    m_spanMs = qint64(std::clamp(seconds, 1, 7 * 86400)) * 1000;
    if (!m_live && m_focusCenter)
        m_viewEnd = std::min(latest(), m_focusCenter + m_spanMs / 2);
    emit viewportChanged();
    scheduleView();
}
void Backend::zoomToRange(qint64 a, qint64 b) {
    if (!m_session || a == b || a < 0 || b < 0)
        return;
    m_viewEnd = std::max(a, b);
    m_spanMs = m_viewEnd - std::min(a, b);
    m_focusSecs = 0;
    m_focusCenter = 0;
    m_live = false;
    emit viewportChanged();
    scheduleView();
}
void Backend::setFocusSeconds(int seconds) {
    m_focusSecs = std::clamp(seconds, -1, 7 * 86400);
    emit viewportChanged();
    scheduleView();
}
void Backend::focusAt(qint64 stamp) {
    if (!m_session || m_focusSecs < 0)
        return;
    if (m_focusSecs == 0)
        m_focusSecs = std::max(1, std::min(60, int(m_spanMs / 5000)));
    m_live = false;
    m_focusCenter = std::clamp(stamp, viewStart(), viewEnd());
    emit viewportChanged();
    scheduleView();
}
void Backend::panBy(qint64 deltaMs) {
    if (!m_session || !deltaMs)
        return;
    const qint64 end = latest();
    auto first = earliest();
    for (auto peer : m_comparisons)
        first = std::min(first, sessionInfo(peer).value("startMs").toLongLong());
    const auto minEnd = std::min(end, first + m_spanMs);
    m_viewEnd = std::clamp(m_viewEnd + deltaMs, minEnd, end);
    m_focusCenter = 0;
    m_live = false;
    emit viewportChanged();
    scheduleView();
}
void Backend::goLive() {
    m_live = true;
    m_focusCenter = 0;
    m_viewEnd = latest();
    emit viewportChanged();
    scheduleView();
}
void Backend::setPixels(int pixels) {
    pixels = std::clamp(pixels, 32, 4096);
    if (pixels == m_pixels)
        return;
    m_pixels = pixels;
    scheduleView();
}
void Backend::scheduleView(bool invalidate) {
    if (!m_ready || !m_session)
        return;
    if (invalidate)
        ++m_version;
    m_pending = true;
    if (!m_busy && !m_coalesce.isActive())
        m_coalesce.start();
}

void Backend::fetchView() {
    if (m_busy || !m_pending)
        return;
    m_busy = true;
    m_pending = false;
    const auto id = m_session, start = viewStart(), end = viewEnd(), fs = focusStart(), fe = focusEnd();
    const auto hops = m_hops;
    const auto version = m_version;
    const auto pixels = m_pixels;
    const auto revision = m_revisions.value(id);
    const auto storageRevision = m_storageRevision;
    QVariantList peers;
    for (auto peer : m_comparisons) {
        auto info = sessionInfo(peer);
        info["revision"] = m_revisions.value(peer);
        peers.append(info);
    }
    QMetaObject::invokeMethod(
        m_readContext, [this, id, start, end, fs, fe, hops, version, pixels, revision, storageRevision, peers] {
            auto &cache = m_chartCache;
            const bool reuse = cache.id == id && cache.start == start && cache.end == end &&
                               cache.hops == hops && cache.pixels == pixels && cache.revision == revision &&
                               cache.storageRevision == storageRevision;
            auto view = m_reader->view(id, start, end, fs, fe, reuse ? QList<int>{} : hops, pixels);
            if (reuse) {
                view["charts"] = cache.charts;
                view["timeMax"] = cache.timeMax;
            } else if (m_reader->error().isEmpty()) {
                cache = {id,
                         start,
                         end,
                         pixels,
                         revision,
                         storageRevision,
                         hops,
                         view.value("charts").toList(),
                         view.value("timeMax").toDouble()};
            }
            QString error = m_reader->error();
            QVariantList charts = view.value("charts").toList();
            double maximum = view.value("timeMax").toDouble();
            QHash<qint64, ChartCache> nextCache;
            for (const auto &value : peers) {
                const auto peer = value.toMap();
                const auto peerId = peer.value("id").toLongLong();
                const auto peerRevision = peer.value("revision").toULongLong();
                auto entry = m_comparisonCache.take(peerId);
                if (entry.id != peerId || entry.start != start || entry.end != end ||
                    entry.pixels != pixels || entry.revision != peerRevision ||
                    entry.storageRevision != storageRevision) {
                    const auto comparison = m_reader->view(peerId, start, end, fs, fe, {0}, pixels, true);
                    if (!m_reader->error().isEmpty()) {
                        error = m_reader->error();
                        break;
                    }
                    entry = {peerId,
                             start,
                             end,
                             pixels,
                             peerRevision,
                             storageRevision,
                             {0},
                             comparison.value("charts").toList(),
                             comparison.value("timeMax").toDouble()};
                }
                auto chart = entry.charts.value(0).toMap();
                chart["measurement"] = m_reader->focusedMeasurement(peerId, fs, fe);
                chart["comparisonId"] = peerId;
                chart["target"] = peer.value("target");
                for (const auto &key : {"location", "resolvedAddress", "clockOffsetMs", "clockErrorMs", "manualCorrectionMs"})
                    chart[key] = peer.value(key);
                chart["family"] = peer.value("family");
                for (const auto &key : {"protocol", "port", "interfaceName", "payloadBytes", "dscp", "dontFragment"})
                    chart[key] = peer.value(key);
                chart["intervalMs"] = peer.value("intervalMs");
                if (!chart.contains("buckets"))
                    chart["buckets"] = QVariantList{};
                charts.append(chart);
                maximum = std::max(maximum, entry.timeMax);
                nextCache.insert(peerId, std::move(entry));
            }
            m_comparisonCache.swap(nextCache);
            view["events"] = m_reader->timelineEvents(id, start, end);
            view["diagnostics"] = m_reader->diagnosticTracks(id, start, end, pixels);
            if (!m_reader->error().isEmpty()) error = m_reader->error();
            view["charts"] = charts;
            view["timeMax"] = maximum;
            view["focusStart"] = fs;
            view["focusEnd"] = fe;
            view["viewStart"] = start;
            view["viewEnd"] = end;
            view["sessionId"] = id;
            QMetaObject::invokeMethod(this, [this, version, view = std::move(view), error]() mutable {
                m_busy = false;
                if (version == m_version) {
                    if (!error.isEmpty())
                        setStatus(error);
                    m_snapshot = std::move(view);
                    refreshNames();
                    emit snapshotChanged();
                    resolveNames();
                }
                if (m_pending)
                    m_coalesce.start();
            });
        });
}

void Backend::refreshNames() {
    auto rows = m_snapshot.value("rows").toList();
    for (auto &row : rows) {
        auto map = row.toMap();
        const auto address = map.value("address").toString();
        if (m_names.contains(address)) {
            map["hostname"] = m_names.value(address);
            row = map;
        }
    }
    if (m_snapshot.contains("rows"))
        m_snapshot["rows"] = rows;
}
void Backend::resolveNames() {
    if (!running())
        return;
    for (const auto &row : m_snapshot.value("rows").toList()) {
        const auto address = row.toMap().value("address").toString();
        if (address.isEmpty() || m_lookups.size() >= 8 || m_names.contains(address) ||
            m_lookups.contains(address) || QHostAddress(address).isNull())
            continue;
        m_lookups.insert(address);
        QHostInfo::lookupHost(address, this, [this, address](const QHostInfo &info) {
            m_lookups.remove(address);
            if (m_names.size() >= 4096)
                m_names.clear();
            const auto name = info.error() == QHostInfo::NoError ? info.hostName() : QString();
            m_names.insert(address, name == address ? QString() : name);
            refreshNames();
            emit snapshotChanged();
        });
    }
}

struct Backend::RouteScan {
    quint64 request;
    qint64 session, cursor = -1;
};

void Backend::cancelRouteHistory() {
    ++m_routeRequest;
}

void Backend::warmRouteIndex(qint64 id, quint64 generation, bool anyUpdated) {
    QMetaObject::invokeMethod(m_writerContext, [this, id, generation, anyUpdated] {
        if (!m_writer)
            return;
        bool more = false, updated = false;
        const bool ok = m_writer->advanceRouteIndex(id, more, &updated);
        const auto error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, id, generation, more, anyUpdated = anyUpdated || updated, ok, error] {
            if (generation != m_indexGeneration)
                return;
            if (!ok) {
                setStatus(error);
                return;
            }
            if (more)
                warmRouteIndex(id, generation, anyUpdated);
            else if (anyUpdated)
                scheduleView(false);
        });
    });
}

void Backend::requestRouteHistory() {
    cancelRouteHistory();
    if (!m_ready || !m_session) {
        emit routeHistoryPage({}, true, m_ready ? QString{} : m_status);
        return;
    }
    auto scan = std::make_shared<RouteScan>();
    scan->request = m_routeRequest;
    scan->session = m_session;
    readRoutePage(std::move(scan));
}

void Backend::readRoutePage(std::shared_ptr<RouteScan> scan) {
    QMetaObject::invokeMethod(m_writerContext, [this, scan] {
        if (!m_writer)
            return;
        bool indexMore = false;
        const bool advanced = m_writer->advanceRouteIndex(scan->session, indexMore);
        if (!advanced) {
            const QString error = m_writer->error();
            QMetaObject::invokeMethod(this, [this, scan, error] {
                if (scan->request != m_routeRequest || scan->session != m_session)
                    return;
                emit routeHistoryPage({}, true, error);
            });
            return;
        }
        if (m_closing.load())
            return;
        QMetaObject::invokeMethod(m_readContext, [this, scan, indexMore] {
            if (!m_reader)
                return;
            QVariantList events;
            bool pageMore = false;
            m_reader->readIndexedRoutes(scan->session, scan->cursor, events, pageMore);
            const QString error = m_reader->error();
            const bool more = indexMore || pageMore;
            QMetaObject::invokeMethod(this, [this, scan, events = std::move(events), more, error] {
                if (scan->request != m_routeRequest || scan->session != m_session)
                    return;
                emit routeHistoryPage(events, !more || !error.isEmpty(), error);
                if (more && error.isEmpty() && scan->request == m_routeRequest)
                    readRoutePage(scan);
            });
        });
    });
}

void Backend::enableRecorder() {
    if (m_recorderAvailable || !m_ready)
        return;
    if (m_background) {
        for (const auto &id : std::as_const(m_resumeSessions))
            runSession(id.toLongLong());
    }
    m_resumeSessions.clear();
    m_recorderAvailable = true;
    emit monitoringChanged();
}

void Backend::persistRecordingSet() {
    if (!m_recorderAvailable || !m_background)
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, active = m_active] {
        if (!m_writer->saveBackground(true, active)) {
            auto error = m_writer->error();
            QMetaObject::invokeMethod(this, [this, error] { setStatus(error); });
        }
    });
}

void Backend::setBackgroundEnabled(bool enabled) {
    if (!m_recorderAvailable || enabled == m_background)
        return;
    QString error;
    // Ensure a close immediately after changing this preference observes its committed value.
    QMetaObject::invokeMethod(m_writerContext, [&] {
        if (!m_writer->saveBackground(enabled, m_active))
            error = m_writer->error();
    }, Qt::BlockingQueuedConnection);
    if (!error.isEmpty()) {
        setStatus(error);
        return;
    }
    m_background = enabled;
    emit monitoringChanged();
}

void Backend::setWindowVisible(bool visible) {
    m_windowVisible = visible;
    m_probe->setPreviewSession(visible ? m_session : 0);
    if (visible) pollReceiver();
    if (visible && m_session) {
        if (m_live) {
            m_viewEnd = latest();
            emit viewportChanged();
        }
        scheduleView();
    }
}

void Backend::setAlertRule(qint64 id, double latency, double loss, int seconds) {
    if (!m_recorderAvailable || sessionInfo(id).isEmpty())
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, id, latency, loss, seconds] {
        if (!m_writer->setAlertRule(id, latency, loss, seconds)) {
            const auto error = m_writer->error();
            QMetaObject::invokeMethod(this, [this, error] { setStatus(error); });
            return;
        }
        QMetaObject::invokeMethod(this, [this, id, latency, loss, seconds] {
            auto key = QString::number(id);
            if (latency || loss)
                m_alertRules[key] = QVariantMap{{"latency", latency}, {"loss", loss}, {"seconds", seconds}};
            else
                m_alertRules.remove(key);
            emit monitoringChanged();
        });
    });
}

void Backend::showAlert(qint64 id, qint64 from, qint64 through) {
    if (sessionInfo(id).isEmpty()) {
        changeTabVisibility(id, false, from, through);
    } else {
        selectSession(id);
        zoomToRange(from, through);
    }
}

void Backend::setDiagnostics(qint64 id, QString url, QString interfaceName, int seconds) {
    const auto source = sessionInfo(id);
    if (!m_ready || m_replayMode || source.isEmpty() || source.value("remote").toBool() ||
        source.value("imported").toBool()) {
        emit diagnosticSettingsSaved(id,
                                     tr("Only local traces can collect service and interface measurements."));
        return;
    }
    url = url.trimmed();
    QMetaObject::invokeMethod(m_writerContext, [this, id, url, interfaceName, seconds] {
        const bool ok = m_writer->saveDiagnostics(id, url, interfaceName, seconds);
        const auto error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, id, url, interfaceName, seconds, error, ok] {
            if (!ok) {
                setStatus(error);
                emit diagnosticSettingsSaved(id, error);
                return;
            }
            const auto key = QString::number(id);
            if (url.isEmpty() && interfaceName.isEmpty())
                m_diagnosticsSettings.remove(key);
            else
                m_diagnosticsSettings[key] =
                    QVariantMap{{"url", url}, {"interface", interfaceName}, {"seconds", seconds}};
            syncDiagnostics(id);
            emit diagnosticsChanged();
            emit diagnosticSettingsSaved(id, {});
        });
    });
}

void Backend::syncDiagnostics(qint64 id) {
    const auto info = sessionInfo(id);
    const bool local = m_active.contains(id) && !m_replayMode && !info.value("remote").toBool() &&
                       !info.value("imported").toBool();
    const auto config = local ? m_diagnosticsSettings.value(QString::number(id)).toMap() : QVariantMap{};
    if (!m_diagnostics && config.isEmpty())
        return;
    if (!m_diagnostics) {
        m_diagnostics = new Diagnostics;
        m_diagnostics->moveToThread(&m_diagnosticThread);
        connect(&m_diagnosticThread, &QThread::finished, m_diagnostics, &QObject::deleteLater);
        // Bound queued observations without stalling interface counter sampling on disk I/O.
        connect(m_diagnostics, &Diagnostics::observation, m_diagnostics,
                [this](qint64 id, QVariantMap record) {
                    constexpr int maxPendingDiagnostics = 256;
                    int pending = m_pendingDiagnostics.load(std::memory_order_relaxed);
                    while (pending < maxPendingDiagnostics &&
                           !m_pendingDiagnostics.compare_exchange_weak(
                               pending, pending + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
                    if (pending >= maxPendingDiagnostics)
                        return;
                    QMetaObject::invokeMethod(
                        m_writerContext,
                        [this, id, record = std::move(record)] {
                            const struct Guard {
                                std::atomic<int> &count;
                                ~Guard() { count.fetch_sub(1, std::memory_order_release); }
                            } guard{m_pendingDiagnostics};
                            if (!m_writer)
                                return;
                            if (!m_writer->appendDiagnostic(id, record)) {
                                const auto error = m_writer->error();
                                QMetaObject::invokeMethod(this, [this, error] { setStatus(error); });
                            } else {
                                QMetaObject::invokeMethod(this, [this, id] {
                                    if (id == m_session)
                                        m_dirty = true;
                                });
                            }
                        },
                        Qt::QueuedConnection);
                });
        m_diagnosticThread.start();
    }
    QMetaObject::invokeMethod(m_diagnostics, [this, id, config, info] {
        m_diagnostics->configure(id, config, info.value("family").toInt(),
                                 info.value("interfaceName").toString());
    });
}
