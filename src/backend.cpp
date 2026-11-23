#include "backend.h"
#include "history.h"
#include "probe.h"
#include <QDateTime>
#include <QHostInfo>
#include <QHostAddress>
#include <algorithm>

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

Backend::Backend(const QString &file, QObject *parent) : QObject(parent) {
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
            if (m_ready)
                m_sessions = m_writer->sessions();
            else
                m_status = m_writer->error();
        },
        Qt::BlockingQueuedConnection);
    if (m_ready) {
        QMetaObject::invokeMethod(
            m_readContext,
            [&] {
                m_reader = std::make_unique<History>(file);
                m_ready = m_reader->open();
                if (!m_ready)
                    m_status = m_reader->error();
            },
            Qt::BlockingQueuedConnection);
    }
    for (auto &entry : m_sessions) {
        auto info = entry.toMap();
        info["running"] = false;
        entry = info;
    }
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
        if (!m_session || (!m_dirty && !running()))
            return;
        m_dirty = false;
        if (m_live) {
            m_viewEnd = latest();
            emit viewportChanged();
            scheduleView(false);
        }
    });
    m_refresh.start();
    if (!m_sessions.isEmpty())
        selectSession(m_sessions.first().toMap().value("id").toLongLong());
}

Backend::~Backend() {
    m_refresh.stop();
    m_coalesce.stop();
    QMetaObject::invokeMethod(m_probe, [this] { m_probe->shutdown(); }, Qt::BlockingQueuedConnection);
    m_collectorThread.quit();
    m_collectorThread.wait();
    // The worker's final samples are already queued ahead of this teardown.
    QMetaObject::invokeMethod(
        m_writerContext,
        [this] {
            flushSamples();
            m_writer.reset();
        },
        Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(m_readContext, [this] { m_reader.reset(); }, Qt::BlockingQueuedConnection);
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
    if (!m_writer->append(batch)) {
        const QString error = m_writer->error();
        QMetaObject::invokeMethod(this, [this, error] {
            recordingError(tr("History write failed; collection paused. %1").arg(error));
        });
        return;
    }
    QHash<qint64, qint64> ends;
    for (const auto &s : batch)
        ends[s.session] = std::max(ends.value(s.session), s.timeMs);
    QMetaObject::invokeMethod(this, [this, ends] {
        for (auto &entry : m_sessions) {
            auto info = entry.toMap();
            const auto id = info.value("id").toLongLong();
            if (!ends.contains(id))
                continue;
            ++m_revisions[id];
            info["endMs"] = std::max(info.value("endMs").toLongLong(), ends[id]);
            entry = info;
        }
        if (ends.contains(m_session)) {
            m_dirty = true;
            if (m_snapshot.isEmpty())
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
    if (running())
        return now();
    const auto info = sessionInfo(m_session);
    return std::max(info.value("startMs").toLongLong(), info.value("endMs").toLongLong());
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
void Backend::setStatus(QString message) {
    if (message == m_status)
        return;
    m_status = std::move(message);
    emit statusChanged();
}

void Backend::startTrace(QString target, int family, int intervalMs) {
    target = target.trimmed();
    if (!m_ready)
        return;
    if (target.isEmpty() || target.size() > 255 || (family != 4 && family != 6 && family != 0) ||
        intervalMs < 250 || intervalMs > 60000) {
        setStatus(tr("Enter a hostname or address and an interval between 0.25 and 60 seconds."));
        return;
    }
    QMetaObject::invokeMethod(m_writerContext, [this, target, family, intervalMs] {
        const auto id = m_writer->createSession(target, family, intervalMs);
        const auto error = m_writer->error();
        const auto stamp = now();
        QMetaObject::invokeMethod(this, [this, id, target, family, intervalMs, error, stamp] {
            if (id <= 0) {
                setStatus(error);
                return;
            }
            m_sessions.prepend(QVariantMap{{"id", id},
                                           {"target", target},
                                           {"family", family},
                                           {"intervalMs", intervalMs},
                                           {"startMs", stamp},
                                           {"endMs", stamp},
                                           {"running", false}});
            selectSession(id);
            runSession(id);
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
    emit sessionsChanged();
}
void Backend::runSession(qint64 id) {
    const auto info = sessionInfo(id);
    if (!m_ready || info.isEmpty())
        return;
    setActive(id, true);
    m_messages[id] = tr("Resolving target…");
    setStatus(m_messages[id]);
    goLive();
    QMetaObject::invokeMethod(m_probe, [this, id, info] {
        m_probe->start(id, info.value("target").toString(), info.value("family").toInt(),
                       info.value("intervalMs").toInt());
    });
}
void Backend::setIntervalMs(int intervalMs) {
    if (!m_session || intervalMs < 250 || intervalMs > 60000)
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
                                       intervalMs);
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
            else if (id == m_session)
                scheduleView();
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
    if (id == m_session || sessionInfo(id).isEmpty())
        return;
    if (m_session)
        m_views[m_session] = {m_hops, m_viewSecs, m_focusSecs, m_live, m_viewEnd, m_focusCenter};
    m_session = id;
    const auto saved = m_views.value(id);
    m_hops = saved.hops;
    m_viewSecs = saved.viewSeconds;
    m_focusSecs = saved.focusSeconds;
    m_live = saved.live;
    m_viewEnd = m_live ? latest() : saved.end;
    m_focusCenter = saved.center;
    m_snapshot.clear();
    emit selectionChanged();
    emit sessionsChanged();
    emit snapshotChanged();
    setStatus(m_messages.value(id, tr("Recorded session")));
    emit viewportChanged();
    scheduleView();
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
        if (chart.toMap().value("destination").toBool() && chart.toMap().value("hop").toInt() == hop) {
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
    m_viewSecs = std::clamp(seconds, 1, 7 * 86400);
    if (!m_live && m_focusCenter)
        m_viewEnd = std::min(latest(), m_focusCenter + qint64(m_viewSecs) * 500);
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
        m_focusSecs = std::max(1, std::min(60, m_viewSecs / 5));
    m_live = false;
    m_focusCenter = std::clamp(stamp, viewStart(), viewEnd());
    emit viewportChanged();
    scheduleView();
}
void Backend::panBy(qint64 deltaMs) {
    if (!m_session || !deltaMs)
        return;
    const qint64 end = latest();
    const auto minEnd = std::min(end, earliest() + qint64(m_viewSecs) * 1000);
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
    QMetaObject::invokeMethod(m_readContext, [this, id, start, end, fs, fe, hops, version, pixels, revision] {
        auto &cache = m_chartCache;
        const bool reuse = cache.id == id && cache.start == start && cache.end == end && cache.hops == hops &&
                           cache.pixels == pixels && cache.revision == revision;
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
                     hops,
                     view.value("charts").toList(),
                     view.value("timeMax").toDouble()};
        }
        view["focusStart"] = fs;
        view["focusEnd"] = fe;
        view["viewStart"] = start;
        view["viewEnd"] = end;
        view["sessionId"] = id;
        const auto error = m_reader->error();
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
