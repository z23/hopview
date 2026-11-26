#pragma once

#include <QHostAddress>
#include <QObject>
#include <functional>

// Results are delivered asynchronously on the parent's thread. Destroy a job or
// its parent to cancel it. A completed job releases itself.
QObject *lookupMacHost(const QString &name, int family, QObject *parent,
                       std::function<void(const QList<QHostAddress> &, const QString &)> done);
QObject *lookupMacPtr(const QHostAddress &address, QObject *parent,
                      std::function<void(const QString &)> done);
