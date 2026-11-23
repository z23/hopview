#include "backend.h"
#include "history.h"
#include "incident.h"
#include <QFileInfo>

void Backend::displayComparison(const QList<qint64> &members, const QList<int> &hops) {
    if (members.isEmpty())
        return;
    for (qsizetype i = m_closedSessions.size(); i-- > 0;) {
        if (members.contains(m_closedSessions[i].toMap().value("id").toLongLong()))
            m_sessions.append(m_closedSessions.takeAt(i));
    }
    selectSession(members.first());
    m_comparisons = members.mid(1);
    m_hops = hops;
    if (m_live)
        m_viewEnd = latest();
    emit sessionsChanged();
    emit viewportChanged();
    scheduleView();
}

void Backend::restoreIncident(const IncidentRange &range) {
    displayComparison(range.sessions, range.pinnedHops);
    zoomToRange(range.from, std::max(range.from + 1, range.through));
}

void Backend::saveGroup(QString name) {
    if (!m_ready || m_replayMode)
        return;
    auto members = m_comparisons;
    members.prepend(m_session);
    const QVariantMap view{{"span", m_spanMs},
                           {"end", m_viewEnd},
                           {"focusSeconds", m_focusSecs},
                           {"center", m_focusCenter},
                           {"live", m_live}};
    QMetaObject::invokeMethod(m_writerContext, [this, name, members, view, hops = m_hops] {
        const bool ok = m_writer->saveGroup(name, members, hops, view);
        const auto error = m_writer->error();
        const auto names = m_writer->groupNames();
        QMetaObject::invokeMethod(this, [this, ok, error, names, name] {
            m_groupNames = names;
            emit groupsChanged();
            setStatus(ok ? tr("Saved comparison group %1.").arg(name) : error);
        });
    });
}

void Backend::openGroup(QString name) {
    if (!m_ready)
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, name] {
        QList<qint64> ids;
        QList<int> hops;
        QVariantMap view;
        const bool ok = m_writer->openGroup(name, ids, hops, &view);
        const auto error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, ok, error, ids, hops, view] {
            if (!ok) {
                setStatus(error);
                return;
            }
            displayComparison(ids, hops);
            if (!view.isEmpty()) {
                m_spanMs = std::max(1LL, view["span"].toLongLong());
                m_live = view["live"].toBool();
                m_viewEnd = m_live ? latest() : view["end"].toLongLong();
                m_focusCenter = view["center"].toLongLong();
                m_focusSecs = view["focusSeconds"].toInt();
                emit viewportChanged();
                scheduleView();
            }
        });
    });
}

void Backend::deleteGroup(QString name) {
    if (!m_ready || m_replayMode)
        return;
    QMetaObject::invokeMethod(m_writerContext, [this, name] {
        const bool ok = m_writer->deleteGroup(name);
        const auto error = m_writer->error();
        const auto names = m_writer->groupNames();
        QMetaObject::invokeMethod(this, [this, ok, error, names] {
            m_groupNames = names;
            emit groupsChanged();
            if (!ok)
                setStatus(error);
        });
    });
}

void Backend::addIncident(QUrl file, QString site, qint64 correctionMs) {
    if (!m_ready || !file.isLocalFile()) {
        emit incidentAdded(false, tr("Choose a local incident file."));
        return;
    }
    const auto selected = m_session;
    const auto path = file.toLocalFile();
    m_exports.start([this, path, site, correctionMs, selected] {
        History dest(m_historyPath);
        IncidentRange range;
        QString error;
        const auto fallback =
            site.trimmed().isEmpty() ? QFileInfo(path).completeBaseName().left(255) : site.trimmed();
        const bool ok = importIncident(path, dest, range, error, fallback, correctionMs);
        QVariantList imported;
        if (ok) {
            for (const auto &record : dest.sessions()) {
                const auto map = record.toMap();
                if (range.sessions.contains(map.value("id").toLongLong()))
                    imported.append(map);
            }
        }
        QMetaObject::invokeMethod(this, [this, ok, error, range, selected, imported] {
            if (!ok) {
                emit incidentAdded(false, error);
                return;
            }
            for (auto item : imported) {
                auto map = item.toMap();
                map.remove("closed");
                map["running"] = false;
                if (sessionInfo(map.value("id").toLongLong()).isEmpty())
                    m_sessions.append(map);
            }
            emit sessionsChanged();
            if (!m_session)
                restoreIncident(range);
            else if (selected == m_session) {
                for (auto id : range.sessions)
                    if (id != m_session && !m_comparisons.contains(id))
                        m_comparisons.append(id);
                zoomToRange(range.from, std::max(range.from + 1, range.through));
            }
            emit incidentAdded(
                true, tr("Added %n recorded trace(s) for comparison.", nullptr, range.sessions.size()));
        });
    });
}
