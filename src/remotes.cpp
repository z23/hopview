#include "backend.h"
#include "collectormanagement.h"
#include "history.h"
#include "receiverendpoint.h"
#include "sqlhelpers.h"
#include <algorithm>

using namespace hopview_sql;

QString Backend::collectorJoinCommand(const QString &address) const {
    telemetry::ReceiverEndpoint endpoint;
    if (!collectorManagementAvailable() || !telemetry::parseReceiverEndpoint(address, endpoint))
        return {};
    const auto argument = address.startsWith('[') ? "'" + address + "'" : address;
    return QStringLiteral("sudo hopview-node join ") + argument;
}

CollectorManagement &Backend::collectorManagement() {
    // The history writer thread serializes collector management access from the desktop.
    if (!m_management)
        m_management = std::make_unique<CollectorManagement>(m_historyPath);
    return *m_management;
}

void Backend::refreshCollectors() {
    if (!m_ready || !collectorManagementAvailable() || m_collectorsLoading)
        return;
    m_collectorsLoading = true;
    QMetaObject::invokeMethod(m_writerContext, [this] {
        auto &management = collectorManagement();
        const auto nodes = management.nodes();
        const auto error = management.error();
        QMetaObject::invokeMethod(this, [this, nodes, error] {
            m_collectorsLoading = false;
            if (!error.isEmpty()) {
                emit collectorListError(error);
                return;
            }
            if (nodes != m_collectors) {
                m_collectors = nodes;
                emit collectorsChanged();
            }
        });
    });
}

void Backend::createCollectorKey() {
    if (!m_ready || !collectorManagementAvailable()) {
        emit collectorKeyCreated({}, tr("Open the receiver history to enroll a collector."));
        return;
    }
    QMetaObject::invokeMethod(m_writerContext, [this] {
        auto &management = collectorManagement();
        const auto key = management.createKey();
        const auto error = management.error();
        QMetaObject::invokeMethod(this, [this, key, error] { emit collectorKeyCreated(key, error); });
    });
}

void Backend::saveCollectorTargets(QString site, qint64 revision, QVariantList targets) {
    if (!m_ready || !collectorManagementAvailable()) {
        emit collectorTargetsSaved(site, tr("Open the receiver history to edit collector targets."));
        return;
    }
    QMetaObject::invokeMethod(m_writerContext, [this, site, revision, targets] {
        auto &management = collectorManagement();
        const bool saved = management.setTargets(site, revision, targets);
        const auto error = saved ? QString{} : management.error();
        QMetaObject::invokeMethod(this, [this, site, error] {
            emit collectorTargetsSaved(site, error);
            refreshCollectors();
        });
    });
}

void Backend::removeCollector(QString site) {
    if (!m_ready || !collectorManagementAvailable()) {
        emit collectorActionDone({}, tr("Open the receiver history to manage collectors."));
        return;
    }
    QMetaObject::invokeMethod(m_writerContext, [this, site] {
        auto &management = collectorManagement();
        const bool ok = management.removeCollector(site);
        const auto error = ok ? QString{} : management.error();
        QMetaObject::invokeMethod(this, [this, site, ok, error] {
            emit collectorActionDone(
                ok ? tr("Removed collector %1. Historical recordings were kept.").arg(site) : QString{}, error);
            refreshCollectors();
        });
    });
}

void Backend::revokeUnusedKeys() {
    if (!m_ready || !collectorManagementAvailable()) {
        emit collectorActionDone({}, tr("Open the receiver history to manage collectors."));
        return;
    }
    QMetaObject::invokeMethod(m_writerContext, [this] {
        auto &management = collectorManagement();
        const int removed = management.revokeUnusedKeys();
        const auto error = removed < 0 ? management.error() : QString{};
        QMetaObject::invokeMethod(this, [this, removed, error] {
            if (removed < 0)
                emit collectorActionDone({}, error);
            else
                emit collectorActionDone(removed ? tr("Revoked %n unused enrollment key(s).", nullptr, removed)
                                                 : tr("No unused enrollment keys to revoke."),
                                         {});
            refreshCollectors();
        });
    });
}

qint64 History::dataVersion() {
    if (!open())
        return -1;
    try {
        Statement version(db_, "PRAGMA data_version");
        return version.next() ? version.integer(0) : -1;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return -1;
    }
}

void Backend::pollReceiver() {
    if (m_remoteBusy || !m_ready || m_replayMode || (!m_windowVisible && !m_background))
        return;
    m_remoteBusy = true;
    const auto after = m_alertEvents.isEmpty() ? 0LL : m_alertEvents.first().toMap().value("id").toLongLong();
    QMetaObject::invokeMethod(m_readContext, [this, after] {
        const auto version = m_reader->dataVersion();
        QVariantList streams, alerts;
        QString error;
        if (version != m_receiverVersion || version < 0) {
            streams = m_reader->sessions(true);
            error = m_reader->error();
            if (error.isEmpty() && !streams.isEmpty()) {
                alerts = m_reader->alertEvents(after);
                error = m_reader->error();
            }
            if (error.isEmpty())
                m_receiverVersion = version;
        }
        QMetaObject::invokeMethod(this, [this, streams, alerts, error] {
            m_remoteBusy = false;
            if (!error.isEmpty()) {
                setStatus(error);
                return;
            }
            bool changed = false, visibleTraceChanged = false;
            for (auto value : streams) {
                auto remote = value.toMap();
                const auto id = remote.value("id").toLongLong();
                const bool closed = remote.take("closed").toBool();
                if (!remote.value("lastReceivedMs").toLongLong())
                    continue;
                remote["running"] = false;
                bool found = false;
                for (auto *list : {&m_sessions, &m_closedSessions}) {
                    auto it = std::find_if(list->begin(), list->end(), [id](const QVariant &s) {
                        return s.toMap().value("id").toLongLong() == id;
                    });
                    if (it == list->end())
                        continue;
                    found = true;
                    if (it->toMap() != remote) {
                        *it = remote;
                        ++m_revisions[id];
                        changed = true;
                        visibleTraceChanged |= id == m_session || m_comparisons.contains(id);
                    }
                    break;
                }
                if (!found) {
                    (closed ? m_closedSessions : m_sessions).prepend(remote);
                    changed = true;
                }
            }
            if (changed) {
                emit sessionsChanged();
                attachSiteStreams();
            }
            if (!m_session && !m_sessions.isEmpty())
                selectSession(m_sessions.first().toMap()["id"].toLongLong());
            if (visibleTraceChanged && m_windowVisible) {
                if (m_live) {
                    m_viewEnd = latest();
                    emit viewportChanged();
                }
                // Backfilled rounds may fall inside a pinned historical selection.
                scheduleView();
            }
            publishAlerts(alerts);
        });
    });
}

QVariantMap Backend::labelAlert(QVariantMap event) const {
    const auto id = event.value("session").toLongLong();
    for (const auto *recordings : {&m_sessions, &m_closedSessions}) {
        for (const auto &entry : *recordings) {
            const auto info = entry.toMap();
            if (info.value("id").toLongLong() != id)
                continue;
            const auto location = info.value("location").toString();
            if (!location.isEmpty())
                event["sourceLabel"] = location + " · " + event.value("target").toString();
            return event;
        }
    }
    return event;
}

void Backend::publishAlerts(const QVariantList &events) {
    if (events.isEmpty())
        return;
    const auto last = m_alertEvents.isEmpty() ? 0LL : m_alertEvents.first().toMap()["id"].toLongLong();
    QVariantList fresh;
    for (const auto &value : events)
        if (value.toMap()["id"].toLongLong() > last)
            fresh.append(labelAlert(value.toMap()));
    if (fresh.isEmpty())
        return;
    m_alertEvents = (fresh + m_alertEvents).mid(0, 200);
    emit alertsChanged();
    for (auto it = fresh.crbegin(); it != fresh.crend(); ++it)
        emit destinationAlert(it->toMap());
}
