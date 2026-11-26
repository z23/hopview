#include "macresolver.h"
#include <QSocketNotifier>
#include <QTimer>
#include <QUrl>
#include <arpa/inet.h>
#include <dns_sd.h>
#include <net/if.h>
#include <utility>

namespace {
// The system resolver replies through a descriptor owned by mDNSResponder.
// Closing our DNSServiceRef cancels the request without joining a worker which
// could still be waiting for a DNS server during application teardown.
class Query : public QObject {
public:
    explicit Query(QObject *parent) : QObject(parent) {
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [this] { finish(); });
    }
    ~Query() override { release(); }

    void watch(DNSServiceErrorType status) {
        const int fd = status == kDNSServiceErr_NoError ? DNSServiceRefSockFD(service) : -1;
        if (fd < 0) {
            deadline.start(0);
            return;
        }
        notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
            const auto status = DNSServiceProcessResult(service);
            if (status != kDNSServiceErr_NoError || ready) finish();
        });
        deadline.start(5000);
    }

    DNSServiceRef service = nullptr;
    QTimer deadline;
    bool ready = false;
    std::function<void()> deliver;

private:
    void release() {
        if (notifier) notifier->setEnabled(false);
        if (service) DNSServiceRefDeallocate(std::exchange(service, nullptr));
    }
    void finish() {
        deadline.stop();
        release();
        // The callback may synchronously close the session that owns this job.
        auto callback = std::move(deliver);
        deleteLater();
        if (callback) callback();
    }
    QSocketNotifier *notifier = nullptr;
};

class HostQuery final : public Query {
public:
    using Query::Query;
    QList<QHostAddress> addresses;
    int family = 0;
    bool answered4 = false, answered6 = false;
    int negativeAnswers = 0;
    static void DNSSD_API response(DNSServiceRef, DNSServiceFlags flags, uint32_t index, DNSServiceErrorType status,
                                   const char *, const sockaddr *address, uint32_t, void *context) {
        auto *q = static_cast<HostQuery *>(context);
        if (status == kDNSServiceErr_NoSuchRecord) {
            q->ready = q->family != 0 || ++q->negativeAnswers >= 2 || !q->addresses.isEmpty();
            return;
        }
        if (status || !address) {
            q->ready = true;
            return;
        }
        const bool v6 = address->sa_family == AF_INET6;
        if (v6) q->answered6 = true;
        else if (address->sa_family == AF_INET) q->answered4 = true;
        else return;
        if (flags & kDNSServiceFlagsAdd) {
            QHostAddress host(address);
            if (host.isLinkLocal() && v6 && index) {
                char name[IF_NAMESIZE]{};
                host.setScopeId(if_indextoname(index, name) ? QString::fromLatin1(name) : QString::number(index));
            }
            if (!q->addresses.contains(host)) q->addresses.append(host);
        }
        if (!(flags & kDNSServiceFlagsMoreComing)) {
            q->ready = q->family != 0 || (q->answered4 && q->answered6) || q->negativeAnswers > 0;
            // Let the other family arrive in the same answer set, but a silent
            // resolver for one family must not hold up a usable answer.
            if (!q->ready && !q->addresses.isEmpty() && q->deadline.remainingTime() > 150)
                q->deadline.start(150);
        }
    }
};

QByteArray ptrName(const QHostAddress &address) {
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        auto ip = address.toIPv4Address();
        QByteArray name;
        for (int i = 0; i < 4; ++i, ip >>= 8) name += QByteArray::number(ip & 255) + '.';
        return name + "in-addr.arpa.";
    }
    const auto ip = address.toIPv6Address();
    const auto hex = QByteArray(reinterpret_cast<const char *>(ip.c), 16).toHex();
    QByteArray name;
    for (auto i = hex.crbegin(); i != hex.crend(); ++i) {
        name += *i;
        name += '.';
    }
    return name + "ip6.arpa.";
}

class PtrQuery final : public Query {
public:
    using Query::Query;
    QString hostname;
    static void DNSSD_API response(DNSServiceRef, DNSServiceFlags flags, uint32_t, DNSServiceErrorType status,
                                   const char *, uint16_t, uint16_t, uint16_t size, const void *data,
                                   uint32_t, void *context) {
        auto *q = static_cast<PtrQuery *>(context);
        q->ready = true;
        if (status || !(flags & kDNSServiceFlagsAdd) || !data || size > 255) return;
        const auto *labels = static_cast<const char *>(data);
        QByteArray name;
        for (int i = 0; i < size;) {
            const int length = quint8(labels[i++]);
            if (!length) {
                if (i == size) q->hostname = QUrl::fromAce(name);
                return;
            }
            if (length > 63 || length > size - i) return;
            if (!name.isEmpty()) name += '.';
            name.append(labels + i, length);
            i += length;
        }
    }
};
} // namespace

QObject *lookupMacHost(const QString &name, int family, QObject *parent,
                       std::function<void(const QList<QHostAddress> &, const QString &)> done) {
    auto *query = new HostQuery(parent);
    query->family = family;
    query->deliver = [query, done = std::move(done)] {
        const auto addresses = query->addresses;
        done(addresses, addresses.isEmpty() ? QObject::tr("No address returned by the system resolver.") : QString{});
    };
    const auto protocols = family == 4 ? kDNSServiceProtocol_IPv4 : family == 6 ? kDNSServiceProtocol_IPv6
                                                                   : kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6;
    const auto host = QUrl::toAce(name);
    query->watch(host.isEmpty() ? kDNSServiceErr_BadParam :
        DNSServiceGetAddrInfo(&query->service, 0, 0, protocols, host.constData(), HostQuery::response, query));
    return query;
}

QObject *lookupMacPtr(const QHostAddress &address, QObject *parent, std::function<void(const QString &)> done) {
    auto *query = new PtrQuery(parent);
    query->deliver = [query, done = std::move(done)] {
        const auto hostname = query->hostname;
        done(hostname);
    };
    const auto scope = address.scopeId().toLatin1();
    bool numeric = false;
    const auto index = scope.toUInt(&numeric);
    const auto interface = numeric || scope.isEmpty() ? index : if_nametoindex(scope.constData());
    query->watch(DNSServiceQueryRecord(&query->service, 0, interface,
                                      ptrName(address).constData(), kDNSServiceType_PTR, kDNSServiceClass_IN,
                                      PtrQuery::response, query));
    return query;
}
