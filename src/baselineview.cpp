#include "backend.h"
#include "history.h"
#include <algorithm>

QVariantMap Backend::savedBaseline() const { return m_baselines.value(m_session); }

void Backend::markBaseline() {
    if (!m_session)
        return;
    m_baselines[m_session] = {{"from", std::max(0LL, focusStart())}, {"through", focusEnd()}};
    emit baselineChanged();
    setStatus(tr("Baseline saved. Select an incident on the timeline, then choose Compare time ranges."));
}

void Backend::compareFocus(bool previous) {
    if (!m_session)
        return;
    const auto incidentFrom = std::max(0LL, focusStart()), incidentEnd = focusEnd();
    const auto saved = savedBaseline();
    const qint64 end = previous || saved.isEmpty() ? incidentFrom - 1 : saved["through"].toLongLong();
    const qint64 from = previous || saved.isEmpty() ? std::max(0LL, end - (incidentEnd - incidentFrom))
                                                    : saved["from"].toLongLong();
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
        if (end < from)
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
