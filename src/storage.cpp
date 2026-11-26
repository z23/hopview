#include "history.h"
#include "historycache.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <algorithm>
#include <array>

using namespace hopview_sql;
namespace {
constexpr qint64 day = 86400000, hour = 3600000;
constexpr std::array<qint64, 3> binSpans{10000, 60000, 3600000};
constexpr std::array<qint64, 3> diagSpans{30000, 300000, 3600000};
qint64 scalar(sqlite3 *db, const char *sql) {
    Statement q(db, sql);
    return q.next() ? q.integer(0) : 0;
}

// Delete expired rows for one non-imported session using each table's leading
// primary-key prefix so SQLite performs bounded range seeks instead of full-table scans.
void pruneSession(sqlite3 *db, qint64 sid, qint64 raw, qint64 summary, bool scoped = false,
                  qint64 from = 0, qint64 through = 0) {
    Statement imp(db, "SELECT 1 FROM imported_sessions WHERE session=?");
    imp.integer(1, sid);
    if (imp.next())
        return;

    QList<int> hops{0};
    {
        Statement hopQ(db, "SELECT hop FROM hops WHERE session=? ORDER BY hop");
        hopQ.integer(1, sid);
        while (hopQ.next())
            hops.append(int(hopQ.integer(0)));
    }

    auto buildPinClause = [&](const char *time) -> QByteArray {
        QByteArray clause = QByteArray(" AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=?1 AND src.") +
                            time + " BETWEEN p.start_ms AND p.end_ms)";
        if (scoped)
            clause += QByteArray(" AND src.") + time + " BETWEEN ?3 AND ?4";
        return clause;
    };

    if (raw) {
        const auto pinT = buildPinClause("t");
        {
            const auto sql = "DELETE FROM samples AS src WHERE src.session=?1 AND src.hop=?5 AND src.t<?2" + pinT;
            Statement del(db, sql.constData());
            for (int hop : hops) {
                del.reset();
                del.integer(1, sid);
                del.integer(2, raw);
                if (scoped) {
                    del.integer(3, from);
                    del.integer(4, through);
                }
                del.integer(5, hop);
                del.next();
            }
        }
        {
            const auto sql = "DELETE FROM probe_failures AS src WHERE src.session=?1 AND src.hop=?5 AND src.t<?2" + pinT;
            Statement del(db, sql.constData());
            for (int hop : hops) {
                del.reset();
                del.integer(1, sid);
                del.integer(2, raw);
                if (scoped) {
                    del.integer(3, from);
                    del.integer(4, through);
                }
                del.integer(5, hop);
                del.next();
            }
        }
        {
            const auto sql =
                "DELETE FROM diagnostic_records AS src WHERE src.session=?1 AND src.kind=?5 AND src.t<?2" + pinT +
                " AND flag=0 AND NOT EXISTS(SELECT 1 FROM diagnostic_bins b WHERE b.session=src.session "
                "AND b.kind=src.kind AND b.span=30000 AND b.b=src.t/30000*30000 AND b.peak_t=src.t)";
            Statement del(db, sql.constData());
            for (const char *kind : {"service", "interface"}) {
                del.reset();
                del.integer(1, sid);
                del.integer(2, raw);
                if (scoped) {
                    del.integer(3, from);
                    del.integer(4, through);
                }
                del.string(5, QString::fromLatin1(kind));
                del.next();
            }
        }
    }

    if (summary) {
        const auto pinT = buildPinClause("t");
        const auto pinB = buildPinClause("b");
        {
            const auto sql =
                "DELETE FROM bins AS src WHERE src.session=?1 AND src.hop=?5 AND src.span=?6 AND src.t<?2" + pinT;
            Statement del(db, sql.constData());
            for (int hop : hops) {
                for (qint64 span : binSpans) {
                    del.reset();
                    del.integer(1, sid);
                    del.integer(2, summary);
                    if (scoped) {
                        del.integer(3, from);
                        del.integer(4, through);
                    }
                    del.integer(5, hop);
                    del.integer(6, span);
                    del.next();
                }
            }
        }
        {
            const auto sql =
                "DELETE FROM diagnostic_records AS src WHERE src.session=?1 AND src.kind=?5 AND src.t<?2" + pinT;
            Statement del(db, sql.constData());
            for (const char *kind : {"service", "interface"}) {
                del.reset();
                del.integer(1, sid);
                del.integer(2, summary);
                if (scoped) {
                    del.integer(3, from);
                    del.integer(4, through);
                }
                del.string(5, QString::fromLatin1(kind));
                del.next();
            }
        }
        {
            const auto sql =
                "DELETE FROM diagnostic_bins AS src WHERE src.session=?1 AND src.kind=?5 AND src.span=?6 AND src.b<?2" +
                pinB;
            Statement del(db, sql.constData());
            for (const char *kind : {"service", "interface"}) {
                for (qint64 span : diagSpans) {
                    del.reset();
                    del.integer(1, sid);
                    del.integer(2, summary);
                    if (scoped) {
                        del.integer(3, from);
                        del.integer(4, through);
                    }
                    del.string(5, QString::fromLatin1(kind));
                    del.integer(6, span);
                    del.next();
                }
            }
        }
        auto pruneSimple = [&](const char *table, const char *extra = "") {
            const auto sql = QByteArray("DELETE FROM ") + table +
                             " AS src WHERE src.session=?1 AND src.t<?2" + pinT + extra;
            Statement del(db, sql.constData());
            del.integer(1, sid);
            del.integer(2, summary);
            if (scoped) {
                del.integer(3, from);
                del.integer(4, through);
            }
            del.next();
        };
        pruneSimple("route_transitions");
        pruneSimple("route_gaps");
        pruneSimple("route_checkpoints",
                    " AND src.t<>(SELECT max(c.t) FROM route_checkpoints c WHERE c.session=src.session AND c.t<?2)"
                    " AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=src.session AND src.t="
                    "(SELECT max(c.t) FROM route_checkpoints c WHERE c.session=p.session AND c.t<p.start_ms))");
        // Preserve the most recent settings before every retained time range.
        pruneSimple("measurement_epochs",
                    " AND src.t<>(SELECT max(e.t) FROM measurement_epochs e WHERE e.session=src.session AND e.t<?2)"
                    " AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=src.session AND src.t="
                    "(SELECT max(e.t) FROM measurement_epochs e WHERE e.session=p.session AND e.t<=p.start_ms))");
    }
}

} // namespace

bool History::setRetention(int rawDays, int summaryDays) {
    if (!open())
        return false;
    error_.clear();
    try {
        if (rawDays < 0 || rawDays > 36500 || summaryDays < 0 || summaryDays > 36500 ||
            (summaryDays && (!rawDays || summaryDays < rawDays)))
            throw std::runtime_error(
                "Summary retention must cover raw retention. Zero keeps data indefinitely.");
        Statement q(db_, "UPDATE retention SET raw_days=?,summary_days=? WHERE id=1");
        q.integer(1, rawDays);
        q.integer(2, summaryDays);
        q.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::pinInterval(QList<qint64> ids, qint64 from, qint64 through) {
    if (!open())
        return false;
    try {
        if (from < 0 || from > through || ids.isEmpty())
            throw std::runtime_error("Invalid interval.");
        Transaction tx(db_, true);
        Statement q(db_, "INSERT OR IGNORE INTO retention_pins VALUES(?,?,?)");
        for (const auto id : ids) {
            if (retainedResolution(id, from, through) != "Raw probes")
                throw std::runtime_error("Raw probes have already expired in part of this interval.");
            q.reset();
            q.integer(1, id);
            q.integer(2, from / hour * hour);
            q.integer(3, through / hour * hour + hour - 1);
            q.next();
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::releasePin(qint64 id, qint64 from, qint64 through) {
    if (!open())
        return false;
    error_.clear();
    try {
        Transaction tx(db_, true);
        Statement erase(db_, "DELETE FROM retention_pins WHERE session=? AND start_ms=? AND end_ms=?");
        erase.integer(1, id);
        erase.integer(2, from);
        erase.integer(3, through);
        erase.next();
        if (sqlite3_changes(db_)) {
            Statement cut(db_, "SELECT raw_before,summary_before FROM retention WHERE id=1");
            cut.next();
            pruneSession(db_, id, cut.integer(0), cut.integer(1), true, from, through);
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QString History::retainedResolution(qint64 id, qint64 from, qint64 through) {
    Statement q(db_, "SELECT raw_before,summary_before,first_ms,last_ms FROM retention,sessions s "
                     "WHERE retention.id=1 AND s.id=?1 AND NOT EXISTS "
                     "(SELECT 1 FROM imported_sessions WHERE session=?1)");
    q.integer(1, id);
    if (!q.next())
        return "Raw probes";
    // Empty time outside the recording does not require discarded measurements.
    qint64 missing = std::max({0LL, from, q.integer(2)});
    const qint64 end = std::min({through, q.integer(0) - 1, q.integer(3)});
    if (missing > end)
        return "Raw probes";
    Statement pins(db_, "SELECT start_ms,end_ms FROM retention_pins WHERE session=?1 AND "
                        "start_ms<=?2 AND end_ms>=?3 ORDER BY start_ms");
    pins.integer(1, id);
    pins.integer(2, end);
    pins.integer(3, missing);
    while (pins.next()) {
        if (pins.integer(0) > missing)
            break;
        missing = std::max(missing, pins.integer(1) + 1);
        if (missing > end)
            return "Raw probes";
    }
    return missing < q.integer(1) ? "Includes expired history" : "Includes 10-second summaries";
}

bool History::maintainStorage(qint64 now) {
    if (!open())
        return false;
    error_.clear();
    try {
        qint64 oldRaw = 0, oldSummary = 0, raw = 0, summary = 0;
        {
            Statement config(db_,
                             "SELECT raw_days,summary_days,raw_before,summary_before FROM retention WHERE id=1");
            config.next();
            auto cutoff = [&](int field) {
                const auto days = config.integer(field);
                return days ? std::max(0LL, (now - days * day) / hour * hour) : 0LL;
            };
            oldRaw = config.integer(2);
            oldSummary = config.integer(3);
            raw = std::max(oldRaw, cutoff(0));
            summary = std::max(oldSummary, cutoff(1));
        }
        Statement behind(db_, "SELECT id FROM sessions WHERE EXISTS(SELECT 1 FROM samples WHERE "
                              "session=sessions.id AND hop=0 AND t<? AND t>coalesce((SELECT cursor "
                              "FROM route_index WHERE session=sessions.id),-1)) LIMIT 1");
        // The next recording whose raw history before this time has not reached the route index.
        auto unindexed = [&](qint64 before) {
            behind.reset();
            behind.integer(1, before);
            const qint64 id = behind.next() ? behind.integer(0) : 0;
            behind.reset();
            return id;
        };
        if (raw > oldRaw) {
            // Upgraded databases may still have unindexed raw history. Save its
            // transitions in bounded transactions before retiring raw observations.
            while (const auto id = unindexed(raw)) {
                Transaction tx(db_, true);
                updateRouteIndex(id, -1);
                tx.commit();
            }
        }
        if (raw > oldRaw || summary > oldSummary) {
            QList<qint64> targets;
            Statement sids(db_, "SELECT id FROM sessions WHERE id NOT IN (SELECT session FROM imported_sessions)");
            while (sids.next())
                targets.append(sids.integer(0));
            // Nothing is stored before the first recording, so the first step can start there.
            // With no recorded samples at all, one step reaches both cutoffs.
            Statement earliest(db_, "SELECT coalesce(min(first_ms),-1) FROM sessions WHERE id NOT IN "
                                    "(SELECT session FROM imported_sessions)");
            earliest.next();
            const qint64 floor = earliest.integer(0) < 0 ? raw : earliest.integer(0) / hour * hour;
            earliest.reset();
            // Retire one hour per transaction and publish its watermark in that same transaction.
            // Readers and late samples then never meet pruned rows above the watermark, a failure
            // leaves the previous hour's consistent state, and a long first prune does not hold
            // the write lock away from live recorders.
            qint64 atRaw = oldRaw, atSummary = oldSummary;
            while (atRaw < raw || atSummary < summary) {
                const qint64 nextRaw = std::min(raw, std::max(atRaw, floor) + hour);
                const qint64 nextSummary = std::min(summary, std::max(atSummary, floor) + hour);
                Transaction tx(db_, true);
                Statement marks(db_, "SELECT raw_before,summary_before FROM retention WHERE id=1");
                marks.next();
                const qint64 pruneRaw = nextRaw > marks.integer(0) ? nextRaw : 0;
                const qint64 pruneSummary = nextSummary > marks.integer(1) ? nextSummary : 0;
                marks.reset();
                if (pruneRaw) {
                    // A late sample may have rewound an index since the pass above.
                    while (const auto id = unindexed(pruneRaw))
                        updateRouteIndex(id, -1);
                    // Each recording's latest detector state must survive its raw samples.
                    execute(db_, "INSERT OR REPLACE INTO route_checkpoints(session,hour,t,state) "
                                 "SELECT session,cursor/3600000,cursor,state FROM route_index WHERE cursor>=0");
                }
                if (pruneRaw || pruneSummary)
                    for (const qint64 sid : targets)
                        pruneSession(db_, sid, pruneRaw, pruneSummary);
                Statement set(db_, "UPDATE retention SET raw_before=max(raw_before,?),"
                                   "summary_before=max(summary_before,?) WHERE id=1");
                set.integer(1, nextRaw);
                set.integer(2, nextSummary);
                set.next();
                tx.commit();
                if (cache_)
                    cache_->routeStates.clear();
                atRaw = nextRaw;
                atSummary = nextSummary;
            }
        }
        Transaction tx(db_, true);
        Statement usage(db_, "INSERT OR IGNORE INTO storage_growth VALUES(?,?)");
        usage.integer(1, now / hour * hour);
        usage.integer(2, (scalar(db_, "PRAGMA page_count") - scalar(db_, "PRAGMA freelist_count")) *
                             scalar(db_, "PRAGMA page_size"));
        usage.next();
        Statement old(db_, "DELETE FROM storage_growth WHERE t<?");
        old.integer(1, now - 7 * day);
        old.next();
        tx.commit();
        // Planner statistics are an optimization. Bound their sampling, because SQLite before
        // 3.46 would analyse whole tables under the write lock, and never fail maintenance on them.
        sqlite3_exec(db_, "PRAGMA analysis_limit=400; PRAGMA optimize", nullptr, nullptr, nullptr);
        return true;
    } catch (const std::exception &e) {
        if (cache_)
            cache_->routeStates.clear();
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantMap History::storageStatus() {
    if (!open())
        return {};
    try {
        Statement q(db_, "SELECT raw_days,summary_days,raw_before,summary_before FROM retention");
        q.next();
        const auto bytes = QFileInfo(path_).size() + QFileInfo(path_ + "-wal").size();
        const auto free = QStorageInfo(QFileInfo(path_).absolutePath()).bytesAvailable();
        Statement growth(db_, "SELECT t,bytes FROM storage_growth ORDER BY t");
        qint64 firstT = 0, firstSize = 0, elapsed = 0, delta = 0;
        while (growth.next()) {
            if (!firstT) {
                firstT = growth.integer(0);
                firstSize = growth.integer(1);
            }
            elapsed = growth.integer(0) - firstT;
            delta = growth.integer(1) - firstSize;
        }
        const double perDay = elapsed ? std::max(0., double(delta) * day / elapsed) : -1.;
        QVariantList pinList;
        Statement pins(db_,
                       "SELECT p.session,p.start_ms,p.end_ms,s.target,coalesce(o.location,'') "
                       "FROM retention_pins p JOIN sessions s ON s.id=p.session "
                       "LEFT JOIN session_origin o ON o.session=s.id ORDER BY p.start_ms DESC,p.session");
        while (pins.next())
            pinList << QVariantMap{{"id", pins.integer(0)},
                                   {"from", pins.integer(1)},
                                   {"through", pins.integer(2)},
                                   {"target", pins.string(3)},
                                   {"site", pins.string(4)}};
        return {{"rawDays", q.integer(0)}, {"summaryDays", q.integer(1)},
                {"bytes", bytes},          {"available", free},
                {"growthPerDay", perDay},  {"daysToFull", perDay > 0 ? free / perDay : -1.},
                {"pins", pinList.size()},  {"pinList", pinList}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

bool History::onlineBackup(const QString &destination) {
    if (!open())
        return false;
    error_.clear();
    QTemporaryFile temp(QFileInfo(destination).absolutePath() + "/.hopview-backup-XXXXXX");
    sqlite3 *copy = nullptr;
    try {
        const QFileInfo target(destination);
        const auto output = target.dir().canonicalPath() + "/" + target.fileName();
        const auto source = QFileInfo(path_).canonicalFilePath();
        for (const auto &suffix : {"", "-wal", "-shm", "-journal"})
            if (output == source + suffix)
                throw std::runtime_error("Choose a backup file outside the live database and its journals.");
        if (destination.isEmpty() || target.exists() || !temp.open())
            throw std::runtime_error("Choose a new backup filename in a writable directory.");
        temp.close();
        const auto rc =
            sqlite3_open_v2(QFile::encodeName(temp.fileName()), &copy, SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK)
            throw std::runtime_error("Cannot open backup destination.");
        // A stable WAL read transaction prevents a busy writer from repeatedly
        // restarting a multi-step backup of a large recording.
        Transaction snapshot(db_, false);
        scalar(db_, "SELECT id FROM retention LIMIT 1");
        auto *backup = sqlite3_backup_init(copy, "main", db_, "main");
        if (!backup)
            throw std::runtime_error(sqlite3_errmsg(copy));
        int result;
        QElapsedTimer busy;
        busy.start();
        do {
            result = sqlite3_backup_step(backup, 256);
            if (result == SQLITE_BUSY || result == SQLITE_LOCKED)
                sqlite3_sleep(20);
            else
                busy.restart();
        } while (result == SQLITE_OK ||
                 ((result == SQLITE_BUSY || result == SQLITE_LOCKED) && busy.elapsed() < 5000));
        const auto finish = sqlite3_backup_finish(backup);
        if (result != SQLITE_DONE || finish != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(copy));
        snapshot.commit();
        sqlite3_close(copy);
        copy = nullptr;
        if (!temp.rename(destination))
            throw std::runtime_error(temp.errorString().toStdString());
        temp.setAutoRemove(false);
        return true;
    } catch (const std::exception &e) {
        if (copy)
            sqlite3_close(copy);
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}
