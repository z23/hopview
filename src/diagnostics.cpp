#include "diagnostics.h"
#include <QDateTime>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QUrl>
#ifdef Q_OS_LINUX
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_mib.h>
#elif defined(Q_OS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif
#include <curl/curl.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <optional>

struct Diagnostics::WifiCache {
    struct Entry {
        quint64 ifindex = 0;
        bool wextUnsupported = false;
        // The device object, the NetworkManager instance that named it, and when to ask again.
        QString nmDevicePath, nmOwner;
        qint64 nmNextLookup = 0;
    };
    QHash<QString, Entry> entries;
};

namespace {
constexpr int interfacePeriodMs = 2000;

std::atomic<int> curlInitState{0};
std::mutex curlInitMutex;

bool ensureCurlInitialized() {
    int state = curlInitState.load(std::memory_order_acquire);
    if (state != 0)
        return state == 1;
    std::lock_guard lock(curlInitMutex);
    state = curlInitState.load(std::memory_order_relaxed);
    if (state != 0)
        return state == 1;
    const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    curlInitState.store(ok ? 1 : -1, std::memory_order_release);
    return ok;
}

#ifdef Q_OS_LINUX
constexpr qint64 nmRetryMs = 60000, nmAbsentRetryMs = 10000;

std::optional<quint64> readNumber(const QString &file) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return {};
    bool ok = false;
    const auto value = input.read(64).trimmed().toULongLong(&ok);
    return ok ? std::optional(value) : std::nullopt;
}

QString networkManagerAP(const QString &interface, qint64 tick, Diagnostics::WifiCache::Entry &cache) {
    // NetworkManager is not running or does not manage this interface. Ask again later.
    if (cache.nmDevicePath.isEmpty() && tick < cache.nmNextLookup)
        return {};
    const auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        cache.nmDevicePath.clear();
        cache.nmNextLookup = tick + nmAbsentRetryMs;
        return {};
    }
    // A late or lost reply says nothing about the device, so only an answer is remembered.
    struct Reply {
        QVariant value;
        QString sender;
        bool answered = false, absent = false;
    };
    const auto call = [&](const QString &path, const QString &iface, const QString &method,
                          const QVariantList &args) {
        auto request = QDBusMessage::createMethodCall("org.freedesktop.NetworkManager", path, iface, method);
        request.setAutoStartService(false);
        request.setArguments(args);
        const auto reply = bus.call(request, QDBus::Block, 200);
        Reply result;
        result.sender = reply.service();
        if (reply.type() == QDBusMessage::ReplyMessage) {
            result.answered = true;
            if (!reply.arguments().isEmpty())
                result.value = reply.arguments().first();
        } else if (reply.type() == QDBusMessage::ErrorMessage) {
            const auto error = QDBusError(reply).type();
            const auto name = reply.errorName();
            result.answered = error != QDBusError::NoReply && error != QDBusError::Timeout &&
                              error != QDBusError::TimedOut;
            result.absent = name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner") ||
                            name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown");
        }
        return result;
    };
    const auto resolveDevice = [&] {
        const auto device = call("/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
                                 "GetDeviceByIpIface", {interface});
        if (!device.answered)
            return false;
        cache.nmDevicePath = device.value.value<QDBusObjectPath>().path();
        cache.nmOwner = device.sender;
        // A stopped NetworkManager usually returns soon. An unmanaged interface rarely changes.
        cache.nmNextLookup = tick + (device.absent ? nmAbsentRetryMs : nmRetryMs);
        return true;
    };
    const auto activeAccessPoint = [&] {
        return call(cache.nmDevicePath, "org.freedesktop.DBus.Properties", "Get",
                    {"org.freedesktop.NetworkManager.Device.Wireless", "ActiveAccessPoint"});
    };
    const auto objectPath = [](const Reply &reply) {
        return reply.value.value<QDBusVariant>().variant().value<QDBusObjectPath>().path();
    };
    if (cache.nmDevicePath.isEmpty() || tick >= cache.nmNextLookup)
        resolveDevice();
    if (cache.nmDevicePath.isEmpty())
        return {};
    auto active = activeAccessPoint();
    // A restarted NetworkManager numbers its objects afresh, so the remembered path can name another
    // device. An answer from a different instance, or one without an access point, means look again.
    if (active.answered && (active.sender != cache.nmOwner || objectPath(active).isEmpty())) {
        if (!resolveDevice() || cache.nmDevicePath.isEmpty())
            return {};
        active = activeAccessPoint();
    }
    const auto ap = objectPath(active);
    if (ap.isEmpty())
        return {};
    if (ap == "/")
        return QStringLiteral("disconnected");
    // Read the address on every poll. A backend that keeps one object per network
    // changes its address in place when the station roams.
    return call(ap, "org.freedesktop.DBus.Properties", "Get",
                {"org.freedesktop.NetworkManager.AccessPoint", "HwAddress"})
        .value.value<QDBusVariant>()
        .variant()
        .toString()
        .toLower();
}

QString accessPoint(const QString &interface, qint64 tick, Diagnostics::WifiCache::Entry &cache) {
    // Drivers with wireless compatibility support expose the associated access point's BSSID here.
    if (!cache.wextUnsupported) {
        const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (sock < 0)
            return networkManagerAP(interface, tick, cache);
        iwreq req{};
        const auto name = interface.toLocal8Bit();
        if (name.size() >= IFNAMSIZ) {
            close(sock);
            return {};
        }
        memcpy(req.ifr_name, name.constData(), name.size());
        const int result = ioctl(sock, SIOCGIWAP, &req);
        const int err = result < 0 ? errno : 0;
        close(sock);
        if (result >= 0) {
            const QByteArray bytes(req.u.ap_addr.sa_data, 6);
            return bytes == QByteArray(6, '\0') ? QStringLiteral("disconnected")
                                                : QString::fromLatin1(bytes.toHex(':'));
        }
        if (err == EOPNOTSUPP || err == ENOTSUP)
            cache.wextUnsupported = true;
    }
    return networkManagerAP(interface, tick, cache);
}

InterfaceReading readInterface(const QString &name, qint64 tick, Diagnostics::WifiCache &wifiCache) {
    InterfaceReading s;
    s.name = name;
    s.tick = tick;
    const auto root = QStringLiteral("/sys/class/net/") + name + "/";
    const auto index = readNumber(root + "ifindex");
    if (!index)
        return s;
    s.index = *index;
    // /proc/net/dev provides one snapshot of all counters in one read.
    QFile counters("/proc/net/dev");
    if (!counters.open(QIODevice::ReadOnly))
        return s;
    for (const auto &line : counters.readAll().split('\n')) {
        const auto sep = line.lastIndexOf(':');
        if (sep < 0 || QString::fromLocal8Bit(line.left(sep).trimmed()) != name)
            continue;
        const auto fields = line.mid(sep + 1).simplified().split(' ');
        if (fields.size() < 16)
            break;
        auto n = [&](int i) { return fields[i].toULongLong(); };
        s.rx = n(0);
        s.tx = n(8);
        s.rxDrops = n(3);
        s.txDrops = n(11);
        s.rxErrors = n(2);
        s.txErrors = n(10);
        s.readable = true;
        break;
    }
    if (auto value = readNumber(root + "carrier"))
        s.carrier = *value ? 1 : 0;
    if (QFileInfo::exists(root + "wireless") || QFileInfo::exists(root + "phy80211")) {
        auto &entry = wifiCache.entries[name];
        if (entry.ifindex != *index) {
            entry = {};
            entry.ifindex = *index;
        }
        s.ap = accessPoint(name, tick, entry);
    }
    return s;
}
#else
InterfaceReading readInterface(const QString &name, qint64 tick, Diagnostics::WifiCache &) {
    InterfaceReading s;
    s.name = name;
    s.tick = tick;
    s.index = QNetworkInterface::interfaceIndexFromName(name);
    if (!s.index)
        return s;
#ifdef Q_OS_MACOS
    int mib[] = {CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, s.index, IFDATA_GENERAL};
    ifmibdata counters{};
    size_t size = sizeof(counters);
    if (sysctl(mib, 6, &counters, &size, nullptr, 0) != 0 || size < sizeof(counters) ||
        name != QString::fromLocal8Bit(counters.ifmd_name))
        return s;
    s.rx = counters.ifmd_data.ifi_ibytes;
    s.tx = counters.ifmd_data.ifi_obytes;
    s.rxErrors = counters.ifmd_data.ifi_ierrors;
    s.txErrors = counters.ifmd_data.ifi_oerrors;
    s.rxDrops = counters.ifmd_data.ifi_iqdrops;
    s.txDrops = counters.ifmd_snd_drops;
    s.readable = true;
#elif defined(Q_OS_WIN)
    MIB_IF_ROW2 counters{};
    counters.InterfaceIndex = s.index;
    if (GetIfEntry2(&counters) != NO_ERROR)
        return s;
    s.rx = counters.InOctets;
    s.tx = counters.OutOctets;
    s.rxErrors = counters.InErrors;
    s.txErrors = counters.OutErrors;
    s.rxDrops = counters.InDiscards;
    s.txDrops = counters.OutDiscards;
    if (counters.MediaConnectState != MediaConnectStateUnknown)
        s.carrier = counters.MediaConnectState == MediaConnectStateConnected;
    s.readable = true;
#endif
    return s;
}
#endif

#ifdef Q_OS_WIN
// libcurl accepts source IPs on Windows but cannot bind by adapter name.
int bindServiceSocket(void *context, curl_socket_t fd, curlsocktype purpose) {
    if (purpose != CURLSOCKTYPE_IPCXN)
        return CURL_SOCKOPT_OK;
    WSAPROTOCOL_INFOW info{};
    int size = sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char *>(&info), &size) != 0)
        return CURL_SOCKOPT_ERROR;
    const auto index = *static_cast<const unsigned *>(context);
    const bool v4 = info.iAddressFamily == AF_INET;
    const DWORD value = v4 ? htonl(index) : index;
    const int result = setsockopt(fd, v4 ? IPPROTO_IP : IPPROTO_IPV6, v4 ? IP_UNICAST_IF : IPV6_UNICAST_IF,
                                  reinterpret_cast<const char *>(&value), sizeof(value));
    return result == 0 ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
}
#endif

int stopTransfer(void *state, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic_bool *>(state)->load(std::memory_order_relaxed) ? 1 : 0;
}
} // namespace

CurlGlobalInit::CurlGlobalInit() { ensureCurlInitialized(); }

CurlGlobalInit::~CurlGlobalInit() {
    std::lock_guard lock(curlInitMutex);
    if (curlInitState.load(std::memory_order_relaxed) == 1) {
        curl_global_cleanup();
        curlInitState.store(0, std::memory_order_release);
    }
}

QVariantMap interfaceSample(const InterfaceReading &s, const InterfaceReading *p, qint64 time) {
    QVariantMap record{
        {"kind", "interface"},  {"t", time},       {"name", s.name},   {"periodMs", interfacePeriodMs},
        {"carrier", s.carrier}, {"ap", s.ap},      {"rx", -1.},        {"tx", -1.},
        {"rxDrops", -1LL},      {"txDrops", -1LL}, {"rxErrors", -1LL}, {"txErrors", -1LL}};
    QStringList events;
    if (!s.readable && (!p || p->readable))
        events << "Interface unavailable";
    if (p && s.readable && !p->readable)
        events << "Interface available";
    if (p && s.readable && p->readable) {
        if (p->index != s.index)
            events << "Interface replaced";
        if (p->carrier >= 0 && s.carrier >= 0 && s.carrier != p->carrier)
            events << (s.carrier ? "Carrier up" : "Carrier down");
        if (!p->ap.isEmpty() && !s.ap.isEmpty() && p->ap != s.ap)
            events << QString("Wi-Fi %1 → %2").arg(p->ap, s.ap);
        const qint64 elapsed = s.tick - p->tick;
        if (p->index == s.index && elapsed > 0 && elapsed <= 3 * interfacePeriodMs) {
            if (s.rx >= p->rx && s.tx >= p->tx && s.rxDrops >= p->rxDrops && s.txDrops >= p->txDrops &&
                s.rxErrors >= p->rxErrors && s.txErrors >= p->txErrors) {
                record["rx"] = (s.rx - p->rx) * 1000. / elapsed;
                record["tx"] = (s.tx - p->tx) * 1000. / elapsed;
                record["rxDrops"] = qint64(s.rxDrops - p->rxDrops);
                record["txDrops"] = qint64(s.txDrops - p->txDrops);
                record["rxErrors"] = qint64(s.rxErrors - p->rxErrors);
                record["txErrors"] = qint64(s.txErrors - p->txErrors);
                if (s.rxDrops > p->rxDrops || s.txDrops > p->txDrops)
                    events << "Packet drops";
                if (s.rxErrors > p->rxErrors || s.txErrors > p->txErrors)
                    events << "Interface errors";
            } else
                events << "Counters reset";
        }
    }
    record["event"] = events.join(" · ");
    return record;
}

QVariantMap measureService(const QString &address, int family, const QString &bindInterface, int seconds,
                           const std::atomic_bool &cancelled, const QByteArray &caFile) {
    const QUrl url(address);
    QVariantMap rec{{"kind", "service"}, {"t", QDateTime::currentMSecsSinceEpoch()},
                    {"url", address},    {"periodMs", seconds * 1000},
                    {"address", ""},     {"family", 0},
                    {"status", 0},       {"dns", -1.},
                    {"connect", -1.},    {"tls", -1.},
                    {"firstByte", -1.},  {"ttfb", -1.},
                    {"total", -1.},      {"error", ""},
                    {"event", ""}};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
        ensureCurlInitialized() ? curl_easy_init() : nullptr, curl_easy_cleanup);
    auto *curl = handle.get();
    if (!curl) {
        rec["error"] = rec["event"] = "Cannot initialize HTTP client";
        return rec;
    }
    const auto encoded = url.toEncoded();
    char message[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_URL, encoded.constData());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE,
                     long(family == 6   ? CURL_IPRESOLVE_V6
                          : family == 4 ? CURL_IPRESOLVE_V4
                                        : CURL_IPRESOLVE_WHATEVER));
#ifdef Q_OS_WIN
    unsigned interfaceIndex = 0;
#endif
    if (!bindInterface.isEmpty()) {
#ifdef Q_OS_WIN
        interfaceIndex = QNetworkInterface::interfaceIndexFromName(bindInterface);
        if (!interfaceIndex) {
            rec["error"] = rec["event"] = "Unknown network interface: " + bindInterface;
            return rec;
        }
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, bindServiceSocket);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, &interfaceIndex);
#else
        const auto device = QByteArray("if!") + bindInterface.toLocal8Bit();
        curl_easy_setopt(curl, CURLOPT_INTERFACE, device.constData());
#endif
    }
    if (!caFile.isEmpty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile.constData());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Hopview");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopTransfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);
    const auto result = curl_easy_perform(curl);
    char *ip = nullptr;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    rec["status"] = int(code);
    if (ip && *ip) {
        rec["address"] = QString::fromLatin1(ip);
        rec["family"] = strchr(ip, ':') ? 6 : 4;
    }
    auto time = [&](CURLINFO info) {
        curl_off_t us = 0;
        curl_easy_getinfo(curl, info, &us);
        return us / 1000.;
    };
    const auto dns = time(CURLINFO_NAMELOOKUP_TIME_T), connected = time(CURLINFO_CONNECT_TIME_T),
               handshake = time(CURLINFO_APPCONNECT_TIME_T), ready = time(CURLINFO_PRETRANSFER_TIME_T),
               first = time(CURLINFO_STARTTRANSFER_TIME_T);
    if (!QHostAddress(url.host()).isNull())
        rec["dns"] = -1.;
    else if (dns > 0 || connected > 0)
        rec["dns"] = dns;
    if (connected > 0)
        rec["connect"] = std::max(0., connected - dns);
    if (handshake > 0)
        rec["tls"] = std::max(0., handshake - connected);
    if (first > 0)
        rec["ttfb"] = first;
    if (first > 0 && ready > 0)
        rec["firstByte"] = std::max(0., first - ready);
    rec["total"] = time(CURLINFO_TOTAL_TIME_T);
    if (result != CURLE_OK)
        rec["error"] = QString::fromUtf8(message[0] ? message : curl_easy_strerror(result)).left(512);
    else if (code >= 400)
        rec["error"] = QString("HTTP %1").arg(code);
    rec["event"] = rec["error"];
    return rec;
}

Diagnostics::Diagnostics() : wifiCache_(std::make_unique<WifiCache>()), timer_(new QTimer(this)) {
    clock_.start();
    pool_.setMaxThreadCount(4);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &Diagnostics::poll);
}
Diagnostics::~Diagnostics() = default;
void Diagnostics::configure(qint64 id, const QVariantMap &settings, int family,
                            const QString &bindInterface) {
    if (auto it = watches_.find(id); it != watches_.end()) {
        it->cancelled->store(true);
        watches_.erase(it);
    }
    if (!settings["url"].toString().isEmpty() || !settings["interface"].toString().isEmpty()) {
        Watch watch;
        watch.url = settings["url"].toString();
        watch.device = settings["interface"].toString();
        watch.family = family;
        watch.bindInterface = bindInterface;
        watch.seconds = settings["seconds"].toInt();
        watch.cancelled = std::make_shared<std::atomic_bool>(false);
        watches_.insert(id, std::move(watch));
    }
    if (watches_.isEmpty())
        timer_->stop();
    else {
        if (!timer_->isActive())
            timer_->start();
        poll();
    }
}
void Diagnostics::shutdown() {
    timer_->stop();
    for (auto &watch : watches_)
        watch.cancelled->store(true);
    pool_.waitForDone();
    watches_.clear();
}
void Diagnostics::poll() {
    const auto tick = clock_.elapsed(), wall = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, InterfaceReading> interfaces;
    QList<std::pair<qint64, qint64>> due;
    for (auto it = watches_.begin(); it != watches_.end(); ++it) {
        auto &watch = it.value();
        const auto id = it.key();
        if (!watch.device.isEmpty() && tick >= watch.nextInterface) {
            if (!interfaces.contains(watch.device))
                interfaces[watch.device] = readInterface(watch.device, tick, *wifiCache_);
            auto current = interfaces.value(watch.device);
            emit observation(
                id,
                interfaceSample(current, watch.previous.name.isEmpty() ? nullptr : &watch.previous, wall));
            watch.previous = std::move(current);
            watch.nextInterface = tick + interfacePeriodMs - 100;
        }
        if (!watch.url.isEmpty() && !watch.busy && tick >= watch.nextCheck)
            due.append({watch.nextCheck, id});
    }
    // Give overdue traces the first available request slot when several services are slow.
    std::sort(due.begin(), due.end());
    for (const auto &entry : std::as_const(due)) {
        const auto id = entry.second;
        auto &watch = watches_[id];
        const auto copy = watch;
        watch.busy = pool_.tryStart([this, id, copy] {
            auto record =
                measureService(copy.url, copy.family, copy.bindInterface, copy.seconds, *copy.cancelled);
            QMetaObject::invokeMethod(this, [this, id, flag = copy.cancelled, record = std::move(record)] {
                if (flag->load())
                    return;
                watches_[id].busy = false;
                emit observation(id, record);
            });
        });
        if (!watch.busy)
            break;
        watch.nextCheck = tick + watch.seconds * 1000;
    }
}
