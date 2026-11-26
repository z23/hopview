#include "nodeconfig.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTextStream>
#include <QUrl>
#include <cmath>

namespace telemetry {
void log(const QString &message) { QTextStream(stderr) << message << Qt::endl; }

bool validSite(const QString &site) {
    static const QRegularExpression pattern("^[A-Za-z0-9][A-Za-z0-9._ -]{0,63}$");
    return pattern.match(site).hasMatch();
}

bool validText(const QString &text, int max, bool allowEmpty) {
    if (text.size() > max || (!allowEmpty && text.trimmed().isEmpty()) || text != text.trimmed())
        return false;
    for (auto ch : text)
        if (!ch.isPrint())
            return false;
    return true;
}

qint64 integer(const QJsonObject &o, const QString &key, qint64 low, qint64 high) {
    const auto v = o.value(key);
    const double d = v.toDouble();
    if (!v.isDouble() || !std::isfinite(d) || std::floor(d) != d || d < low || d > high)
        throw Error("Invalid integer: " + key);
    return v.toInteger();
}

Trace parseTrace(const QJsonObject &o) {
    const auto target = o.value("target").toString();
    int family = o.contains("family") ? int(integer(o, "family", 0, 6)) : 0;
    auto protocol = o.value("protocol").toString("auto");
    int port = o.contains("port") ? int(integer(o, "port", 0, 65535)) : 0;
    auto iface = o.value("interface").toString();
    if (o.contains("dontFragment") && !o["dontFragment"].isBool()) throw Error("Invalid fragmentation setting");
    ProbeOptions options{protocol, port, iface,
        int(o.contains("payloadBytes") ? integer(o,"payloadBytes",8,65507) : 16),
        int(o.contains("dscp") ? integer(o,"dscp",0,63) : 0), o["dontFragment"].toBool()};
    if (!validText(target, 255) || !validText(iface, 63, true) ||
        (family != 0 && family != 4 && family != 6) || !options.valid())
        throw Error("Invalid target, family, protocol, interface or port");
    QVariantMap diagnostics;
    if (o.contains("diagnostics")) {
        if (!o["diagnostics"].isObject()) throw Error("Expected diagnostic settings");
        const auto settings = o["diagnostics"].toObject();
        const auto address = settings["url"].toString();
        const QUrl url(address, QUrl::StrictMode);
        const auto device = settings["interface"].toString();
        if ((!address.isEmpty() && (address.size() > 2048 || !url.isValid() || url.host().isEmpty() ||
             (url.scheme() != "http" && url.scheme() != "https") || !url.userInfo().isEmpty() || url.hasFragment())) ||
            !validText(device, 63, true) || device.contains('/') || device == "." || device == "..")
            throw Error("Invalid service URL or diagnostic interface");
        const auto seconds = settings.contains("seconds") ? integer(settings, "seconds", 5, 3600) : 30;
        if (!address.isEmpty() || !device.isEmpty())
            diagnostics = {{"url", address}, {"interface", device}, {"seconds", seconds}};
    }
    return {target, family, int(o.contains("intervalMs") ? integer(o, "intervalMs", 250, 60000) : 1000),
            options, diagnostics};
}

QJsonObject traceObject(const Trace &t) {
    QJsonObject result{{"target", t.target},
            {"family", t.family},
            {"intervalMs", t.intervalMs},
            {"protocol", t.probes.protocol},
            {"port", t.probes.destinationPort()},
            {"interface", t.probes.interfaceName}};
    // Omit defaults so existing collector stream identities remain stable.
    if (t.probes.payloadBytes != 16) result["payloadBytes"] = t.probes.payloadBytes;
    if (t.probes.dscp) result["dscp"] = t.probes.dscp;
    if (t.probes.dontFragment) result["dontFragment"] = true;
    if (!t.diagnostics.isEmpty()) result["diagnostics"] = QJsonObject::fromVariantMap(t.diagnostics);
    return result;
}

QString filePath(const QString &base, const QJsonObject &o, const QString &key) {
    const auto path = o.value(key).toString();
    if (!validText(path, 4096))
        throw Error("Expected file path for " + key);
    return QDir(base).absoluteFilePath(path);
}
QByteArray readFile(const QString &path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error(path + ": " + file.errorString());
    auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFile::NoError)
        throw Error("Cannot read bounded file " + path);
    return bytes;
}
QByteArray readToken(const QString &path) {
    const auto token = readFile(path, 1024).trimmed();
    if (token.size() < 32 || token.size() > 256)
        throw Error("Token must contain 32 through 256 printable ASCII bytes");
    for (char c : token)
        if (c < 33 || c > 126)
            throw Error("Token contains whitespace or non-ASCII characters");
    return token;
}
bool plainLoopback(const QJsonObject &o, const QString &host) {
    if (!o.contains("insecureLoopback"))
        return false;
    if (!o.value("insecureLoopback").isBool())
        throw Error("insecureLoopback must be a boolean");
    if (!o.value("insecureLoopback").toBool())
        return false;
    if (!QHostAddress(host).isLoopback())
        throw Error("Plaintext requires a literal loopback address");
    return true;
}
QSslConfiguration tlsConfig(const QJsonObject &o, const QString &base, bool server) {
    if (!QSslSocket::supportsSsl())
        throw Error("Qt TLS support is unavailable");
    auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    ssl.setPeerVerifyMode(server ? QSslSocket::VerifyNone : QSslSocket::VerifyPeer);
    if (server) {
        auto certs = QSslCertificate::fromData(readFile(filePath(base, o, "certificate"), 1 << 20));
        auto data = readFile(filePath(base, o, "privateKey"), 1 << 20);
        QSslKey key(data, QSsl::Rsa);
        if (key.isNull())
            key = QSslKey(data, QSsl::Ec);
        if (certs.isEmpty() || key.isNull())
            throw Error("Invalid PEM certificate chain or RSA/EC private key");
        ssl.setLocalCertificateChain(certs);
        ssl.setPrivateKey(key);
    } else if (o.contains("caCertificate")) {
        auto certs = QSslCertificate::fromData(readFile(filePath(base, o, "caCertificate"), 1 << 20));
        if (certs.isEmpty())
            throw Error("Invalid CA certificate");
        ssl.setCaCertificates(certs);
    }
    return ssl;
}
} // namespace telemetry
