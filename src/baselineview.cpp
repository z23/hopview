#include "backend.h"
#include "history.h"
#include <algorithm>
#include <QDateTime>

QVariantMap Backend::savedBaseline() const { return m_baselines.value(m_session); }

void Backend::markBaseline(QString name) {
    const auto id = m_session;
    name = name.trimmed();
    if (!id || name.isEmpty() || name.size() > 100)
        return;
    m_namedBaselines[id][name] = {
        {"name", name}, {"from", std::max(0LL, focusStart())}, {"through", focusEnd()}, {"p95", -1.}};
    selectBaseline(name);
}

void Backend::selectBaseline(QString name) {
    const auto id = m_session;
    const auto mark = m_namedBaselines.value(id).value(name);
    if (mark.isEmpty())
        return;
    m_baselines[id] = mark;
    emit baselineChanged();
    QMetaObject::invokeMethod(m_readContext, [=, this] {
        const auto from = mark["from"].toLongLong(), through = mark["through"].toLongLong();
        const auto p95 = mark["p95"].toDouble() >= 0 ? mark["p95"].toDouble()
                                                     : m_reader->destinationP95(id, from, through);
        const bool ok = (mark["p95"].toDouble() >= 0 || m_reader->error().isEmpty()) &&
                        m_reader->storeBaseline(id, name, from, through, p95);
        const auto error = m_reader->error();
        QMetaObject::invokeMethod(this, [=, this] {
            if (!ok) {
                setStatus(error);
                return;
            }
            if (m_namedBaselines.value(id).value(name) != mark)
                return;
            m_namedBaselines[id][name]["p95"] = p95;
            if (m_baselines.value(id) == mark) {
                m_baselines[id]["p95"] = p95;
                if (id == m_session)
                    emit baselineChanged();
            }
        });
    });
}

void Backend::compareYesterday() { compareIntervals(focusStart() - 86400000, focusEnd() - 86400000); }

void Backend::compareFocus(bool previous) {
    if (!m_session)
        return;
    const auto incidentFrom = std::max(0LL, focusStart()), incidentEnd = focusEnd();
    const auto saved = savedBaseline();
    const qint64 end = previous || saved.isEmpty() ? incidentFrom - 1 : saved["through"].toLongLong();
    const qint64 from = previous || saved.isEmpty() ? std::max(0LL, end - (incidentEnd - incidentFrom))
                                                    : saved["from"].toLongLong();
    compareIntervals(from, end);
}

void Backend::compareIntervals(qint64 from, qint64 end) {
    const auto incidentFrom = std::max(0LL, focusStart()), incidentEnd = focusEnd();
    const auto request = ++m_baselineVersion;
    const auto session = m_session;
    m_baselinePool.clear();
    m_baselinePool.start([=, this] {
        const auto cancelled = [=, this] { return m_baselineVersion.load() != request; };
        if (cancelled())
            return;
        History h(m_historyPath);
        QVariantList rows;
        QString error;
        if (from < 0 || end < from)
            error = tr("The selection has no preceding time range.");
        else {
            rows = h.comparePeriods(session, from, end, incidentFrom, incidentEnd, cancelled);
            error = h.error();
        }
        QMetaObject::invokeMethod(this, [=, this] {
            if (!cancelled() && session == m_session)
                emit comparisonReady(rows, from, end, incidentFrom, incidentEnd, error);
        });
    });
}
