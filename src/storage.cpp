#include "history.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <algorithm>

using namespace hopview_sql;
namespace {
constexpr qint64 day = 86400000, hour = 3600000;
qint64 scalar(sqlite3 *db, const char *sql) {
    Statement q(db, sql);
    return q.next() ? q.integer(0) : 0;
}
constexpr char protectedRow[] =
    " AND session NOT IN (SELECT session FROM imported_sessions)"
    " AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=src.session AND ";
void pruneHistory(sqlite3 *db, qint64 raw, qint64 summary, qint64 session = 0, qint64 from = 0,
                  qint64 through = 0) {
    auto prune = [&](const char *table, const char *time, qint64 end, const char *extra = "") {
        const auto sql = QByteArray("DELETE FROM ") + table + " AS src WHERE " + time + "<?1" + protectedRow +
                         "src." + time + " BETWEEN p.start_ms AND p.end_ms)" + extra;
        const auto scoped =
            session ? sql + " AND src.session=?2 AND src." + time + " BETWEEN ?3 AND ?4" : sql;
        Statement del(db, scoped.constData());
        del.integer(1, end);
        if (session) {
            del.integer(2, session);
            del.integer(3, from);
            del.integer(4, through);
        }
        del.next();
    };
    if (raw) {
        prune("samples", "t", raw);
        prune("probe_failures", "t", raw);
        prune("diagnostic_records", "t", raw,
              " AND flag=0 AND NOT EXISTS(SELECT 1 FROM diagnostic_bins b WHERE b.session=src.session "
              "AND b.kind=src.kind "
              "AND b.span=30000 AND b.b=src.t/30000*30000 AND b.peak_t=src.t)");
    }
    if (summary) {
        for (auto table : {"bins", "diagnostic_records", "route_transitions", "route_gaps"})
            prune(table, "t", summary);
        prune("diagnostic_bins", "b", summary);
        prune("route_checkpoints","t",summary,
              " AND t<>(SELECT max(t) FROM route_checkpoints c WHERE c.session=src.session AND t<?1)"
              " AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=src.session AND src.t="
              "(SELECT max(t) FROM route_checkpoints c WHERE c.session=p.session AND c.t<p.start_ms))");
        // Preserve the most recent settings before every retained time range.
        prune("measurement_epochs", "t", summary,
              " AND t<>(SELECT max(t) FROM measurement_epochs e WHERE e.session=src.session AND t<?1)"
              " AND NOT EXISTS(SELECT 1 FROM retention_pins p WHERE p.session=src.session AND src.t="
              "(SELECT max(t) FROM measurement_epochs e WHERE e.session=p.session AND e.t<=p.start_ms))");
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
            pruneHistory(db_, cut.integer(0), cut.integer(1), id, from, through);
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
        Transaction tx(db_, true);
        Statement config(db_,
                         "SELECT raw_days,summary_days,raw_before,summary_before FROM retention WHERE id=1");
        config.next();
        const auto cutoff = [&](int field) {
            const auto days = config.integer(field);
            return days ? std::max(0LL, (now - days * day) / hour * hour) : 0LL;
        };
        const auto raw = std::max(config.integer(2), cutoff(0)),
                   summary = std::max(config.integer(3), cutoff(1));
        if (raw > config.integer(2)) {
            // Upgraded databases may still have unindexed raw history. Save its
            // transitions before retiring the only observations that can produce them.
            Statement behind(db_, "SELECT id FROM sessions WHERE EXISTS(SELECT 1 FROM samples WHERE "
                                  "session=sessions.id AND hop=0 AND t<? AND t>coalesce((SELECT cursor "
                                  "FROM route_index WHERE session=sessions.id),-1)) LIMIT 1");
            while (true) {
                behind.reset();
                behind.integer(1,raw);
                if (!behind.next()) break;
                const auto id=behind.integer(0);
                behind.reset();
                updateRouteIndex(id,-1);
            }
        }
        pruneHistory(db_, raw > config.integer(2) ? raw : 0, summary > config.integer(3) ? summary : 0);
        Statement set(db_, "UPDATE retention SET raw_before=?,summary_before=? WHERE id=1");
        set.integer(1, raw);
        set.integer(2, summary);
        set.next();
        Statement usage(db_, "INSERT OR IGNORE INTO storage_growth VALUES(?,?)");
        usage.integer(1, now / hour * hour);
        usage.integer(2, (scalar(db_, "PRAGMA page_count") - scalar(db_, "PRAGMA freelist_count")) *
                             scalar(db_, "PRAGMA page_size"));
        usage.next();
        Statement old(db_, "DELETE FROM storage_growth WHERE t<?");
        old.integer(1, now - 7 * day);
        old.next();
        tx.commit();
        return true;
    } catch (const std::exception &e) {
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
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        constexpr auto pathCase = Qt::CaseInsensitive;
#else
        constexpr auto pathCase = Qt::CaseSensitive;
#endif
        for (const auto &suffix : {"", "-wal", "-shm", "-journal"})
            if (output.compare(source + suffix, pathCase) == 0)
                throw std::runtime_error("Choose a backup file outside the live database and its journals.");
        if (destination.isEmpty() || target.exists() || !temp.open())
            throw std::runtime_error("Choose a new backup filename in a writable directory.");
        temp.close();
        const auto rc =
            sqlite3_open_v2(temp.fileName().toUtf8(), &copy, SQLITE_OPEN_READWRITE, nullptr);
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
