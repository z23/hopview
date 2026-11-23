#pragma once
#include <QString>

class History;

struct IncidentRange {
    qint64 from = 0;
    qint64 through = 0;
};

// The caller supplies a disposable history. A failed import is discarded in full.
bool importIncident(const QString &path, History &history, IncidentRange &range, QString &error);
