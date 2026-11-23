#pragma once
#include <QString>

namespace telemetry {
struct EnrollmentKey {
    QString secret;
    QByteArray certificateDigest;
};

EnrollmentKey parseEnrollmentKey(const QString &text);
int joinCollector(const QString &endpoint);
}
