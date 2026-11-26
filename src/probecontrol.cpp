#include "probesession.h"

Probe::Probe(QObject *parent) : QObject(parent) {}

void Probe::start(qint64 id, QString target, int family, int intervalMs, QString protocol, int port, QString interfaceName, int payloadBytes, int dscp, bool dontFragment) {
    stop(id);
    auto *s = newProbeSession(this, id, intervalMs, {std::move(protocol), port, std::move(interfaceName), payloadBytes, dscp, dontFragment});
    sessions_.insert(id, s);
    s->resolve(std::move(target), family);
}

void Probe::stop(qint64 id) {
    if (auto *s = sessions_.take(id)) {
        s->cancel();
        s->deleteLater();
    }
}

void Probe::finish(qint64 id) {
    if (auto *s = sessions_.value(id)) s->finish();
}

void Probe::shutdown() {
    for (auto id : sessions_.keys()) stop(id);
}
