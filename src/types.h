#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

struct Sample {
    qint64 session = 0;
    qint64 timeMs = 0;
    int hop = 0;
    QString address;
    double rttMs = -1;
    bool destination = false;
};
Q_DECLARE_METATYPE(Sample)
