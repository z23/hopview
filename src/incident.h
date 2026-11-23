#pragma once
#include <QString>
#include <QList>

class History;

struct IncidentRange {
    qint64 from = 0;
    qint64 through = 0;
    QList<qint64> sessions; // Primary first, then the tracks shown below it.
    QList<int> pinnedHops{0};
};

// Parse evidence in a disposable database, then atomically adopt the validated rows.
bool importIncident(const QString &path, History &history, IncidentRange &range, QString &error,
                    const QString &site = {}, qint64 correctionMs = 0);
