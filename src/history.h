#pragma once

#include "types.h"
#include <QList>
#include <QVariantMap>

struct sqlite3;

// Each worker owns its connection. All timestamps are Unix milliseconds.
class History {
  public:
    explicit History(QString path);
    ~History();
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    bool open();
    QString error() const;
    qint64 createSession(QString target, int family, int intervalMs);
    QVariantList sessions();
    bool setTabClosed(qint64 session, bool closed);
    bool setInterval(qint64 session, int intervalMs);
    bool addNote(qint64 session, qint64 timeMs, QString text);
    bool append(const QVector<Sample> &samples);
    QVariantMap view(qint64 session, qint64 viewStart, qint64 viewEnd, qint64 focusStart, qint64 focusEnd,
                     QList<int> selectedHops, int pixels);

  private:
    QString path_;
    QString error_;
    sqlite3 *db_ = nullptr;
};
