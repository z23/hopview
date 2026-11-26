#pragma once

#include "routeevents.h"
#include "sqlhelpers.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QVariantMap>
#include <array>
#include <memory>
#include <optional>

struct History::Cache {
    struct RouteEntry {
        qint64 cursor = -1;
        qint64 lastCheckpoint = -1;
        QString state; // As stored in route_index, to notice another connection's rebuild.
        RouteEvents detector;
    };

    explicit Cache(sqlite3 *db) : db_(db) {}

    hopview_sql::Statement &stmt(std::optional<hopview_sql::Statement> &slot, const char *sql) {
        if (!slot)
            slot.emplace(db_, sql);
        else
            slot->recover();
        return *slot;
    }

    sqlite3 *db_;

    // Append hot-path statements.
    std::optional<hopview_sql::Statement> insertSample;
    std::optional<hopview_sql::Statement> insertFailure;
    std::optional<hopview_sql::Statement> setDestination;
    std::optional<hopview_sql::Statement> watermark;
    std::optional<hopview_sql::Statement> preserved;
    std::optional<hopview_sql::Statement> saveBin;
    std::optional<hopview_sql::Statement> upsertHop;
    std::optional<hopview_sql::Statement> updateSession;

    // Reader / sweep statements (one per rollup level so nested sweeps never reset outer cursors).
    std::array<std::optional<hopview_sql::Statement>, 3> binsLevel;
    std::array<std::optional<hopview_sql::Statement>, 3> chartBinsLevel;
    std::optional<hopview_sql::Statement> rawRange;
    std::optional<hopview_sql::Statement> chartRawRange;
    std::optional<hopview_sql::Statement> isImported;
    std::optional<hopview_sql::Statement> sessionPins;
    std::optional<hopview_sql::Statement> dataVersionStmt;
    std::optional<hopview_sql::Statement> identityLengths;
    std::optional<hopview_sql::Statement> capturedIdentity;
    std::optional<hopview_sql::Statement> inventoryMatch;
    std::optional<hopview_sql::Statement> originMatch;

    // Network identity lookup cache, invalidated automatically via PRAGMA data_version or local writes.
    qint64 identityDataVersion = -1;
    bool lengthsLoaded = false;
    QHash<int, QList<int>> inventoryLengths;
    QHash<int, QList<int>> originLengths;
    QHash<QPair<qint64, QString>, QVariantMap> identityMap;

    // In-memory route detector state for sequential appends without JSON re-parsing.
    QHash<qint64, RouteEntry> routeStates;
};
