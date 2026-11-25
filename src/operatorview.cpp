#include "backend.h"
#include "collectormanagement.h"
#include "history.h"
#include "probeoptions.h"
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHostAddress>
#include <QTimeZone>
#include <algorithm>

namespace {
QString utc(qint64 stamp) {
    return QDateTime::fromMSecsSinceEpoch(stamp, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString latency(double value) {
    return value < 0 ? QStringLiteral("unavailable") : QString::number(value, 'f', 1) + " ms";
}

QString siteName(const QVariantMap &info) {
    const auto site = info.value("location").toString();
    return site.isEmpty() ? QStringLiteral("Local desktop") : site;
}

QString queryLabel(const QVariantMap &info) {
    const int family = info.value("family").toInt();
    const auto method = info.value("protocol", "auto").toString().toUpper();
    QString label = QString("%1 · %2 · %3").arg(info.value("target").toString(),
        family ? QString("IPv%1").arg(family) : "Auto IP", method);
    if (method == "AUTO") label += QString(" (fallback UDP/%1)").arg(info.value("port").toInt());
    else if (method != "ICMP") label += QString("/%1").arg(info.value("port").toInt());
    const auto iface = info["interfaceName"].toString();
    if (!iface.isEmpty()) label += QString(" · %1").arg(iface);
    if (info.value("dscp").toInt()) label += QString(" · DSCP %1").arg(info["dscp"].toInt());
    if (info.value("dontFragment").toBool()) label += " · DF";
    if (info.value("payloadBytes",16).toInt() != 16)
        label += QString(" · %1 B").arg(info["payloadBytes"].toInt());
    return label;
}

QString pathDescription(const QVariantMap &settings) {
    if (settings.value("mixed").toBool()) return "Effective settings changed in the interval";
    if (settings.value("effectiveProtocol").toString().isEmpty()) return "Effective settings unavailable";
    QString value = QString("IPv%1 %2").arg(settings["effectiveFamily"].toInt())
                        .arg(settings["effectiveProtocol"].toString().toUpper());
    if (settings["effectivePort"].toInt() > 0) value += QString("/%1").arg(settings["effectivePort"].toInt());
    if (!settings["resolvedAddress"].toString().isEmpty()) value += " to " + settings["resolvedAddress"].toString();
    if (!settings["sourceAddress"].toString().isEmpty()) value += " from " + settings["sourceAddress"].toString();
    if (!settings["effectiveInterface"].toString().isEmpty()) value += " via " + settings["effectiveInterface"].toString();
    if (!settings["flowMode"].toString().isEmpty()) value += " (" + settings["flowMode"].toString() + ")";
    if (settings["partial"].toBool()) value += "; earlier settings unavailable";
    return value;
}
}

void Backend::startTracePair(QString target, int firstFamily, int secondFamily, int intervalMs,
                             QString protocol, int port, QString iface, bool df,
                             int firstSize, int secondSize, int firstDscp, int secondDscp) {
    target = target.trimmed();
    if (m_replayMode || !m_ready) return;
    if (firstFamily != secondFamily && !QHostAddress(target).isNull()) {
        setStatus(tr("Enter a hostname for a dual-stack comparison."));
        return;
    }
    const ProbeOptions options{protocol, port, iface, firstSize, firstDscp, df};
    const ProbeOptions other{protocol, port, iface, secondSize, secondDscp, df};
    const auto validFamily = [](int family) { return family == 0 || family == 4 || family == 6; };
    if (!options.valid() || !other.valid() || !validFamily(firstFamily) || !validFamily(secondFamily) ||
        target.isEmpty() || target.size() > 255 || intervalMs < 250 || intervalMs > 60000) {
        setStatus(tr("Choose a valid target and probe profiles for both traces."));
        return;
    }
    QVariantMap first{{"target", target}, {"protocol", protocol}, {"port", options.destinationPort()},
                      {"intervalMs", intervalMs}, {"interfaceName", iface}, {"dontFragment", df},
                      {"family", firstFamily}, {"payloadBytes", firstSize}, {"dscp", firstDscp}};
    auto second = first;
    second["family"] = secondFamily;
    second["payloadBytes"] = secondSize;
    second["dscp"] = secondDscp;
    if (first == second) {
        setStatus(tr("Select two different probe profiles to compare."));
        return;
    }
    m_pairSettings = {first, second};
    m_pairIds = {0,0};
    const auto recordings = m_sessions;
    for (const auto &s : recordings)
        if (s.toMap()["running"].toBool()) matchPair(s.toMap()["id"].toLongLong());
    if (m_pairSettings.isEmpty()) return;
    if (!m_pairIds[0]) startTrace(target, firstFamily, intervalMs, protocol, port, iface, true, firstSize, firstDscp, df);
    if (!m_pairIds[1]) startTrace(target, secondFamily, intervalMs, protocol, port, iface, true, secondSize, secondDscp, df);
}

void Backend::matchPair(qint64 id) {
    if (m_pairSettings.isEmpty()) return;
    const auto s = sessionInfo(id);
    for (int i = 0; i < m_pairSettings.size(); ++i) {
        const auto &spec = m_pairSettings[i];
        bool matches = true;
        for (auto it = spec.begin(); matches && it != spec.end(); ++it) {
            QVariant fallback;
            if (it.key() == "payloadBytes") fallback = 16;
            if (it.key() == "dscp") fallback = 0;
            if (it.key() == "dontFragment") fallback = false;
            matches = s.value(it.key(), fallback) == it.value();
        }
        if (matches) m_pairIds[i] = id;
    }
    if (!m_pairIds.contains(0)) {
        m_pairSettings.clear();
        displayComparison(m_pairIds, {0});
        goLive();
    }
}

void Backend::compareSessions(QVariantList ids, qint64 from, qint64 through) {
    QList<qint64> members;
    QSet<qint64> available;
    for (const auto &s : m_sessions + m_closedSessions)
        available.insert(s.toMap()["id"].toLongLong());
    for (const auto &v : ids) {
        const auto id = v.toLongLong();
        if (available.contains(id) && !members.contains(id)) members.append(id);
    }
    if (members.isEmpty()) return;
    displayComparison(members, {0});
    if (through > from && from >= 0) zoomToRange(from, through);
}

void Backend::compareSites() {
    if (!m_session) return;
    const auto current = sessionInfo(m_session);
    const QString key = queryLabel(current);
    QMap<QString, QVariantMap> newest;
    for (const auto &v : m_sessions + m_closedSessions) {
        const auto info = v.toMap();
        const QString site = siteName(info);
        if (site == siteName(current) || key != queryLabel(info) ||
            info["startMs"].toLongLong() > viewEnd() || (!m_live && info["endMs"].toLongLong() < viewStart())) continue;
        if (newest.value(site)["endMs"].toLongLong() < info["endMs"].toLongLong()) newest[site] = info;
    }
    if (newest.isEmpty()) {
        setStatus(tr("No other site recorded this target with the same probe settings in this time range."));
        return;
    }
    const auto from = viewStart(), through = viewEnd();
    const bool live = m_live;
    QList<qint64> ids{m_session};
    for (const auto &info : newest) ids.append(info["id"].toLongLong());
    displayComparison(ids, {0});
    if (live) goLive();
    else zoomToRange(from, through);
}

QVariantList Backend::comparisonSites() const {
    const auto primary = sessionInfo(m_session);
    if (primary.isEmpty()) return {};
    QMap<QString, QVariantMap> choices;
    const auto label = queryLabel(primary);
    for (const auto &v : m_sessions + m_closedSessions) {
        const auto info = v.toMap();
        const auto site = siteName(info);
        if (site == siteName(primary) || queryLabel(info) != label ||
            (!m_live && (info["startMs"].toLongLong() > viewEnd() || info["endMs"].toLongLong() < viewStart()))) continue;
        auto &entry = choices[site];
        entry["site"] = site;
        if (info["endMs"].toLongLong() >= entry["lastMs"].toLongLong()) {
            entry["lastMs"] = info["endMs"];
            entry["recording"] = info["id"];
        }
    }
    if (collectorManagementAvailable()) {
        for (const auto &v : m_collectors) {
            const auto node = v.toMap();
            const auto site = node["site"].toString();
            if (site == siteName(primary)) continue;
            auto &choice = choices[site];
            choice["site"] = site;
            choice["managed"] = true;
            choice["revision"] = node["revision"];
            choice["targets"] = node["targets"];
            for (const auto &old : node["targets"].toList()) {
                auto configured = old.toMap();
                configured["interfaceName"] = configured["interface"];
                const ProbeOptions options{configured["protocol"].toString(), configured["port"].toInt(), {}};
                configured["port"] = options.destinationPort();
                if (queryLabel(configured) == label && configured["intervalMs"] == primary["intervalMs"])
                    choice["assigned"] = true;
            }
        }
    }
    QVariantList rows;
    for (auto &choice : choices) rows.append(choice);
    return rows;
}

void Backend::applySiteComparison(QVariantList selectedSites) {
    const auto primary = sessionInfo(m_session);
    if (primary.isEmpty() || selectedSites.isEmpty()) return;
    const auto selectedId = m_session;
    const auto request = ++m_siteRequest;
    const auto list = comparisonSites();
    QVariantList additions, peers;
    QSet<QString> waiting;
    QStringList problems;
    for (const auto &selected : selectedSites) {
        const auto pick = selected.toMap();
        const auto site = pick["site"].toString();
        const auto it = std::find_if(list.cbegin(), list.cend(), [&](const QVariant &v) { return v.toMap()["site"] == site; });
        if (it == list.cend()) { problems << tr("%1 is no longer available.").arg(site); continue; }
        const auto choice = it->toMap();
        if (choice.value("recording").toLongLong()) {
            peers << choice["recording"];
        } else if (choice["assigned"].toBool()) {
            waiting.insert(site);
        } else if (choice["managed"].toBool() && pick["assign"].toBool() &&
                   pick["revision"].toLongLong() == choice["revision"].toLongLong()) {
            additions << choice;
        } else {
            problems << tr("%1 changed. Review the site again.").arg(site);
        }
    }
    QVariantMap target{{"target",primary["target"]},{"family",primary["family"]},{"protocol",primary["protocol"]},
        {"port",primary["port"]},{"intervalMs",primary["intervalMs"]},{"interface",primary["interfaceName"]}};
    if (primary.value("payloadBytes",16).toInt() != 16) target["payloadBytes"] = primary["payloadBytes"];
    if (primary["dscp"].toInt()) target["dscp"] = primary["dscp"];
    if (primary["dontFragment"].toBool()) target["dontFragment"] = true;
    QMetaObject::invokeMethod(m_readContext, [=, this]() mutable {
        for (const auto &v : additions) {
            const auto choice = v.toMap();
            const auto site = choice["site"].toString();
            auto targets = choice["targets"].toList();
            targets.append(target);
            auto &manager = collectorManagement();
            if (manager.setTargets(site, choice["revision"].toLongLong(), targets)) waiting.insert(site);
            else problems << site + ": " + manager.error();
        }
        QMetaObject::invokeMethod(this, [=, this] {
            if (!additions.isEmpty()) refreshCollectors();
            if (request != m_siteRequest || selectedId != m_session) {
                emit siteComparisonApplied(tr("Site requests finished. The active trace changed."));
                return;
            }
            QList<qint64> members{m_session};
            for (const auto &v : peers) members << v.toLongLong();
            displayComparison(members, {0});
            m_sitePrimary = selectedId;
            m_waitingSites = waiting;
            if (!waiting.isEmpty() && !m_live) goLive();
            attachSiteStreams();
            QString message;
            if (!m_waitingSites.isEmpty())
                message = tr("Waiting for measurements from %1.").arg(QStringList(m_waitingSites.begin(), m_waitingSites.end()).join(", "));
            else if (members.size() > 1) message = tr("Showing %1 sites.").arg(members.size());
            const auto outcome = (QStringList{message} + problems).join(" ").trimmed();
            setStatus(outcome);
            emit siteComparisonApplied(outcome);
        });
    });
}

void Backend::attachSiteStreams() {
    if (m_waitingSites.isEmpty()) return;
    if (m_sitePrimary != m_session) { m_waitingSites.clear(); return; }
    auto ids = m_comparisons;
    for (const auto &v : comparisonSites()) {
        const auto site = v.toMap();
        const auto id = site["recording"].toLongLong();
        if (id && m_waitingSites.remove(site["site"].toString()) && !ids.contains(id)) ids.append(id);
    }
    if (ids == m_comparisons) return;
    ids.prepend(m_session);
    displayComparison(ids, {0});
}

void Backend::requestFleet(qint64 from, qint64 through) {
    if (!m_ready || from < 0 || through < from) return;
    const auto request = ++m_fleetRequest;
    const auto recordings = m_sessions + m_closedSessions;
    m_exports.start([this, recordings, from, through, request] {
        History reader(m_historyPath);
        if (!reader.openReadOnly()) {
            const auto error = reader.error();
            QMetaObject::invokeMethod(this, [=, this] {
                if (request == m_fleetRequest.load()) emit fleetReady({}, from, through, error);
            });
            return;
        }
        QVariantList cells;
        QMap<QPair<QString,QString>, QVariantMap> latest;
        QString error;
        // Prefer the latest recording with evidence before the window end for each site/profile.
        for (const auto &v : recordings) {
            if (request != m_fleetRequest.load()) return;
            auto info = v.toMap();
            if (info["startMs"].toLongLong() > through) continue;
            info["lastSampleMs"] = reader.lastDestinationSampleAt(info["id"].toLongLong(), through);
            if (!reader.error().isEmpty()) { error = reader.error(); break; }
            const auto site = siteName(info), target = queryLabel(info);
            info["siteLabel"] = site;
            info["targetLabel"] = target;
            const auto key = qMakePair(site, target);
            const auto previous = latest.value(key);
            if (previous.isEmpty() || info["lastSampleMs"].toLongLong() > previous["lastSampleMs"].toLongLong() ||
                (info["lastSampleMs"] == previous["lastSampleMs"] && info["startMs"].toLongLong() > previous["startMs"].toLongLong()))
                latest.insert(key, info);
        }
        for (auto cell : latest) {
            if (request != m_fleetRequest.load()) return;
            const auto periods = reader.destinationPeriods(cell["id"].toLongLong(), from, through);
            if (!reader.error().isEmpty()) { error = reader.error(); break; }
            for (auto it = periods.begin(); it != periods.end(); ++it) cell[it.key()] = it.value();
            cells.append(cell);
        }
        QMetaObject::invokeMethod(this, [=, this] {
            if (request == m_fleetRequest.load()) emit fleetReady(cells, from, through, error);
        });
    });
}

void Backend::copyIncidentSummary() {
    if (!m_session || !m_ready) return;
    const auto from = std::max(0LL, focusStart()), through = focusEnd();
    if (through < from) return;
    const auto mark = savedBaseline();
    const auto baseFrom = mark.value("from", std::max(0LL, from - (through - from + 1))).toLongLong();
    const auto baseEnd = mark.value("through", from - 1).toLongLong();
    QVariantList members{sessionInfo(m_session)};
    for (auto id : std::as_const(m_comparisons)) members.append(sessionInfo(id));
    setStatus(tr("Preparing incident summary…"));
    m_exports.start([=, this] {
        History reader(m_historyPath);
        QStringList lines{QString("Hopview incident  %1 – %2").arg(utc(from), utc(through)),
                          QString("Baseline  %1 – %2").arg(utc(baseFrom), utc(baseEnd))};
        QString error;
        if (!reader.openReadOnly()) error = reader.error();
        for (const auto &v : members) {
            if (!error.isEmpty()) break;
            const auto info = v.toMap();
            const auto id = info["id"].toLongLong();
            QVariantList comparison;
            QVariantMap dst;
            if (reader.retainedResolution(id,from,through) == "Raw probes" &&
                reader.retainedResolution(id,baseFrom,baseEnd) == "Raw probes") {
                comparison = reader.comparePeriods(id, baseFrom, baseEnd, from, through, [] { return false; });
                dst = comparison.value(0).toMap();
            } else {
                dst = reader.destinationPeriods(id,from,through,baseFrom,baseEnd);
            }
            if (!reader.error().isEmpty()) { error = reader.error(); break; }
            const auto view = reader.view(id, from, through, from, through, {}, 1);
            if (!reader.error().isEmpty()) { error = reader.error(); break; }
            const auto incident = dst["incident"].toMap(), baseline = dst["baseline"].toMap();
            lines << QString() << siteName(info) + "  ·  " + queryLabel(info);
            const auto settings = view["measurement"].toMap();
            lines << pathDescription(settings);
            for (const auto &name : {"baseline","incident"}) {
                const auto resolution = dst[name].toMap()["resolution"].toString();
                if (!resolution.isEmpty()) lines << QString("%1 evidence: %2. Counts cover retained buckets. Exact p95 unavailable.").arg(name,resolution);
            }
            if (!info["location"].toString().isEmpty())
                lines << (settings.value("clockErrorMs", -1).toLongLong() >= 0 ?
                    QString("Collector clock offset %1 ms ± %2 ms").arg(settings["clockOffsetMs"].toLongLong()).arg(settings["clockErrorMs"].toLongLong()) :
                    QStringLiteral("Clock alignment in interval unknown"));
            lines << QString("Destination replies %1/%2, p95 %3 (baseline %4/%5, p95 %6).")
                         .arg(incident["received"].toLongLong()).arg(incident["sent"].toLongLong())
                         .arg(latency(incident.value("p95", -1).toDouble()))
                         .arg(baseline["received"].toLongLong()).arg(baseline["sent"].toLongLong())
                         .arg(latency(baseline.value("p95", -1).toDouble()));
            lines << view["impact"].toMap()["description"].toString();
            for (const auto &entry : comparison) {
                const auto row = entry.toMap();
                if (row["changed"].toBool())
                    lines << QString("Hop %1 responder set: %2 → %3.").arg(row["hop"].toInt())
                            .arg(row["baseline"].toMap()["addresses"].toStringList().join(", "),
                                 row["incident"].toMap()["addresses"].toStringList().join(", "));
            }
            const auto events = reader.timelineEvents(id, from, through);
            if (!reader.error().isEmpty()) { error = reader.error(); break; }
            int count = 0;
            for (const auto &e : events) {
                const auto event = e.toMap();
                if (event["kind"] != "route") continue;
                if (++count <= 8) lines << utc(event["t"].toLongLong()) + "  " + event["text"].toString();
            }
            if (events.size() >= 501) lines << QString("Confirmed route changes among the latest events: %1.").arg(count);
            else lines << QString("Indexed route changes in incident: %1.").arg(count);
        }
        const auto text = lines.join('\n');
        QMetaObject::invokeMethod(this, [this, text, error] {
            if (error.isEmpty()) QGuiApplication::clipboard()->setText(text);
            setStatus(error.isEmpty() ? tr("Incident summary copied.") : error);
            emit incidentSummaryReady(error.isEmpty() ? text : QString(), error);
        });
    });
}
