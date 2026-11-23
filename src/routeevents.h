#pragma once

#include <QMap>
#include <QVariantList>
#include <QVector>
#include <deque>

struct RouteRound {
    qint64 timeMs;
    QMap<int, QString> replies;
    int destinationHop = 0;
    double destinationRtt = -1;
};

// State survives read pages, including a candidate crossing a page boundary.
class RouteEvents {
  public:
    QVariantMap observe(const RouteRound &round);

  private:
    struct Hop {
        QString address, candidate;
        int streak = 0;
    };
    struct Outcome { qint64 stamp; double rtt; };
    QVector<Hop> hops_;
    int destination_ = 0, destinationCandidate_ = 0, destinationStreak_ = 0;
    QString targetAddress_;
    qint64 previousIntervalMs_ = 60000;
    std::deque<Outcome> recent_;
};
