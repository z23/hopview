#include "backend.h"
#include "history.h"
#include <QDateTime>
#include <algorithm>

void Backend::requestResponders(int hop) {
    const auto id = m_session, from = focusStart(), through = focusEnd();
    QMetaObject::invokeMethod(m_readContext, [=, this] {
        auto rows = m_reader->responders(id, hop, from, through);
        auto error = m_reader->error();
        QMetaObject::invokeMethod(this, [=, this] { emit respondersReady(id, hop, rows, error); });
    });
}

void Backend::importInventory(QUrl file) {
    if (!file.isLocalFile() || m_replayMode)
        return;
    m_exports.start([=, this] {
        History writer(m_historyPath);
        const bool ok = writer.importInventory(file.toLocalFile());
        const auto error = writer.error();
        QMetaObject::invokeMethod(this, [=, this] {
            setStatus(ok ? tr("Network inventory imported.") : error);
            if (ok)
                scheduleView();
        });
    });
}

void Backend::storageAction(QString action, int rawDays, int summaryDays, QUrl file) {
    if (m_replayMode)
        return;
    const auto from = std::max(0LL, focusStart()), through = focusEnd(), id = m_session;
    auto ids = m_comparisons;
    if (id)
        ids.prepend(id);
    // Backups and expiry use a dedicated connection so a long snapshot does not
    // occupy the recording writer or the interactive viewport reader.
    m_exports.start([=, this] {
        History store(m_historyPath);
        bool ok = store.open();
        QString message;
        if (ok && action == "save") {
            ok = store.setRetention(rawDays, summaryDays) &&
                 store.maintainStorage(QDateTime::currentMSecsSinceEpoch());
            message = tr("Retention applied.");
        } else if (ok && action == "pin") {
            ok = store.pinInterval(ids, from, through);
            message = tr("Selected interval pinned for all comparison traces.");
        } else if (ok && action == "backup") {
            ok = file.isLocalFile() && store.onlineBackup(file.toLocalFile());
            message = tr("Backup saved.");
        }
        if (!ok)
            message = store.error();
        const auto status = store.storageStatus();
        QMetaObject::invokeMethod(this, [=, this] {
            emit storageUpdated(status, message);
            if (ok && action == "save") {
                ++m_storageRevision;
                scheduleView();
            }
        });
    });
}

void Backend::releasePin(qint64 id, qint64 from, qint64 through) {
    if (m_replayMode) return;
    m_exports.start([=, this] {
        History store(m_historyPath);
        const bool ok = store.releasePin(id, from, through);
        const auto message = ok ? tr("Pin released. Retention applied to this interval.") : store.error();
        const auto status = store.storageStatus();
        QMetaObject::invokeMethod(this, [=, this] {
            emit storageUpdated(status, message);
            if (ok) { ++m_revisions[id]; scheduleView(); }
        });
    });
}
