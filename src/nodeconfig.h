#pragma once

#include "probeoptions.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QString>
#include <stdexcept>

namespace telemetry {
constexpr int maxFrame = 256 * 1024;
constexpr int defaultPort = 8347;
constexpr qint64 maxStamp = 253402300799999LL; // Last millisecond of year 9999.

struct Error : std::runtime_error {
    explicit Error(const QString &s) : std::runtime_error(s.toStdString()) {}
};

struct Trace {
    QString target;
    int family, intervalMs;
    ProbeOptions probes;
};

bool validSite(const QString &site);
bool validText(const QString &text, int max, bool allowEmpty = false);
qint64 integer(const QJsonObject &o, const QString &key, qint64 low, qint64 high);
Trace parseTrace(const QJsonObject &o);
QJsonObject traceObject(const Trace &trace);
QString filePath(const QString &base, const QJsonObject &o, const QString &key);
QByteArray readFile(const QString &path, qint64 limit);
QByteArray readToken(const QString &path);
void log(const QString &message);
QSslConfiguration tlsConfig(const QJsonObject &o, const QString &base, bool server);
bool plainLoopback(const QJsonObject &o, const QString &host);

int collect(const QJsonObject &config, const QString &base);
int receive(const QJsonObject &config, const QString &base);
} // namespace telemetry
