#include "collectormanagement.h"
#include "nodeconfig.h"
#include "sqlhelpers.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QUrl>
#include <array>

using namespace hopview_sql;

namespace {
constexpr qint64 maxRevision = 9007199254740991LL;
constexpr char schema[] = R"SQL(
CREATE TABLE IF NOT EXISTS managed_collectors(
    key_digest TEXT PRIMARY KEY, node_id TEXT UNIQUE, hostname TEXT, site TEXT UNIQUE,
    targets TEXT NOT NULL DEFAULT '[]', revision INTEGER NOT NULL DEFAULT 1,
    applied_revision INTEGER NOT NULL DEFAULT 0, apply_error TEXT NOT NULL DEFAULT '',
    last_seen_ms INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS receiver_certificate(
    id INTEGER PRIMARY KEY CHECK (id=1), fingerprint TEXT NOT NULL
);
)SQL";

bool isHexDigest(const QString &value) {
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    return digest.match(value).hasMatch();
}

QString keyDigest(const QString &secret) {
    return QString::fromLatin1(QCryptographicHash::hash(secret.toLatin1(), QCryptographicHash::Sha256).toHex());
}

bool locationTaken(sqlite3 *db, const QString &site, const QStringList &reservedSites) {
    if (reservedSites.contains(site))
        return true;
    Statement used(db, "SELECT 1 FROM managed_collectors WHERE site=?");
    used.string(1, site);
    if (used.next())
        return true;
    Statement history(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='receiver_streams'");
    if (!history.next())
        return false;
    Statement past(db, "SELECT 1 FROM receiver_streams WHERE location=? LIMIT 1");
    past.string(1, site);
    return past.next();
}

QString listJson(const QVariantList &entries) {
    if (entries.size() > 128)
        throw telemetry::Error("A collector can have at most 128 targets.");
    QJsonArray targets;
    QSet<QByteArray> unique;
    for (const auto &entry : entries) {
        const auto object = QJsonObject::fromVariantMap(entry.toMap());
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() == "interface" || it.key() == "protocol") {
                if (!it.value().isString())
                    throw telemetry::Error("Invalid target option: " + it.key());
            }
        }
        const auto trace = telemetry::parseTrace(object);
        const QHostAddress literal(trace.target);
        if (literal.isNull()) {
            static const QRegularExpression hostname(
                "^(?=.{1,253}\\.?$)(?:[a-z0-9_](?:[a-z0-9_-]{0,61}[a-z0-9_])?\\.)*"
                "[a-z0-9_](?:[a-z0-9_-]{0,61}[a-z0-9_])?\\.?$", QRegularExpression::CaseInsensitiveOption);
            if (!hostname.match(QString::fromLatin1(QUrl::toAce(trace.target))).hasMatch())
                throw telemetry::Error("Expected a hostname or IP address.");
        } else if ((trace.family == 4 && literal.protocol() != QAbstractSocket::IPv4Protocol) ||
                   (trace.family == 6 && literal.protocol() != QAbstractSocket::IPv6Protocol)) {
            throw telemetry::Error("Target address does not match the selected IP family.");
        }
        auto target = telemetry::traceObject(trace);
        auto identity = target;
        identity["target"] = literal.isNull() ? trace.target.toCaseFolded() : literal.toString();
        const auto key = QJsonDocument(identity).toJson(QJsonDocument::Compact);
        if (unique.contains(key))
            throw telemetry::Error("Target list contains a duplicate.");
        unique.insert(key);
        target["port"] = trace.probes.port;
        targets.append(target);
    }
    const auto serialized = QJsonDocument(targets).toJson(QJsonDocument::Compact);
    if (serialized.size() > telemetry::maxFrame - 1024)
        throw telemetry::Error("Target list exceeds the transfer limit.");
    return QString::fromUtf8(serialized);
}
}

CollectorManagement::CollectorManagement(QString databasePath) : path_(std::move(databasePath)) {}
CollectorManagement::~CollectorManagement() { sqlite3_close(db_); }

void CollectorManagement::open() {
    error_.clear();
    if (db_)
        return;
    try {
        const auto path = QFile::encodeName(path_);
        const auto result = sqlite3_open_v2(path.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        check(db_, result);
        check(db_, sqlite3_busy_timeout(db_, 4000));
        execute(db_, schema);
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

void CollectorManagement::setCertificateFingerprint(const QString &fingerprint) {
    open();
    if (!fingerprint.isEmpty() && !isHexDigest(fingerprint))
        throw telemetry::Error("Invalid receiver certificate digest.");
    Statement save(db_, "INSERT OR REPLACE INTO receiver_certificate(id,fingerprint) VALUES(1,?)");
    save.string(1, fingerprint);
    save.next();
}

QString CollectorManagement::createKey() {
    try {
        open();
        Transaction tx(db_, true);
        std::array<quint32, 8> words;
        QRandomGenerator::system()->fillRange(words.data(), words.size());
        const auto secret = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(words.data()),
                                                          sizeof(words)).toHex());
        Statement insert(db_, "INSERT INTO managed_collectors(key_digest) VALUES(?)");
        insert.string(1, keyDigest(secret));
        insert.next();
        Statement cert(db_, "SELECT fingerprint FROM receiver_certificate WHERE id=1");
        const auto fingerprint = cert.next() ? cert.string(0) : QString{};
        tx.commit();
        return QStringLiteral("hv1.") + secret + (fingerprint.isEmpty() ? QString{} : "." + fingerprint);
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

QVariantList CollectorManagement::nodes() {
    try {
        open();
        QVariantList result;
        Statement rows(db_, "SELECT site,hostname,revision,applied_revision,apply_error,last_seen_ms,targets "
                            "FROM managed_collectors WHERE node_id IS NOT NULL ORDER BY site COLLATE NOCASE,site");
        while (rows.next())
            result.append(QVariantMap{{"site", rows.string(0)}, {"hostname", rows.string(1)},
                {"revision", rows.integer(2)}, {"appliedRevision", rows.integer(3)}, {"applyError", rows.string(4)},
                {"lastSeenMs", rows.integer(5)}, {"targets", QJsonDocument::fromJson(rows.string(6).toUtf8()).array().toVariantList()}});
        return result;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return {};
    }
}

bool CollectorManagement::setTargets(const QString &site, qint64 expectedRevision, const QVariantList &targets) {
    try {
        open();
        const auto encoded = listJson(targets);
        Transaction tx(db_, true);
        Statement row(db_, "SELECT revision,targets FROM managed_collectors WHERE site=?");
        row.string(1, site);
        if (!row.next())
            throw telemetry::Error("Unknown collector.");
        if (row.integer(0) != expectedRevision)
            throw telemetry::Error("The target list changed. Reload it before saving.");
        if (row.string(1) != encoded) {
            Statement update(db_, "UPDATE managed_collectors SET targets=?,revision=revision+1,apply_error='' "
                                  "WHERE site=?");
            update.string(1, encoded);
            update.string(2, site);
            update.next();
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool CollectorManagement::hasSite(const QString &site) {
    open();
    Statement existing(db_, "SELECT 1 FROM managed_collectors WHERE site=?");
    existing.string(1, site);
    return existing.next();
}

QJsonObject CollectorManagement::enroll(const QString &secret, const QString &nodeId, const QString &hostname,
                                        const QStringList &reservedSites, qint64 appliedRevision,
                                        const QString &applyError, qint64 nowMs) {
    open();
    const QUuid uuid(nodeId);
    if (!isHexDigest(secret) || uuid.isNull() || !telemetry::validText(hostname, 253))
        throw telemetry::Error("Authentication failed");
    const auto id = uuid.toString(QUuid::WithoutBraces);
    const auto digest = keyDigest(secret);
    Transaction tx(db_, true);
    Statement key(db_, "SELECT site,node_id FROM managed_collectors WHERE key_digest=?");
    key.string(1, digest);
    if (!key.next() || (!key.string(1).isEmpty() && key.string(1) != id))
        throw telemetry::Error("Authentication failed");
    auto site = key.string(0);
    if (site.isEmpty()) {
        Statement claimed(db_, "SELECT 1 FROM managed_collectors WHERE node_id=?");
        claimed.string(1, id);
        if (claimed.next())
            throw telemetry::Error("Authentication failed");
        static const QRegularExpression punctuation("[^A-Za-z0-9._-]+");
        auto base = hostname.left(64).replace(punctuation, "-");
        if (!telemetry::validSite(base))
            base = "collector";
        site = base;
        for (int number = 2; locationTaken(db_, site, reservedSites); ++number) {
            const auto suffix = "-" + QString::number(number);
            site = base.left(64 - suffix.size()) + suffix;
        }
    }
    Statement bind(db_, "UPDATE managed_collectors SET node_id=?,hostname=?,site=? WHERE key_digest=?");
    bind.string(1, id);
    bind.string(2, hostname);
    bind.string(3, site);
    bind.string(4, digest);
    bind.next();
    const auto state = recordStatus(site, appliedRevision, applyError, nowMs);
    tx.commit();
    return state;
}

QJsonObject CollectorManagement::heartbeat(const QString &site, qint64 appliedRevision, const QString &applyError,
                                          qint64 nowMs) {
    open();
    Transaction tx(db_, true);
    const auto state = recordStatus(site, appliedRevision, applyError, nowMs);
    tx.commit();
    return state;
}

QJsonObject CollectorManagement::recordStatus(const QString &site, qint64 appliedRevision, const QString &applyError,
                                              qint64 nowMs) {
    if (appliedRevision < 0 || appliedRevision > maxRevision || !telemetry::validText(applyError, 256, true))
        throw telemetry::Error("Invalid collector status");
    Statement row(db_, "SELECT revision,targets FROM managed_collectors WHERE site=?");
    row.string(1, site);
    if (!row.next())
        throw telemetry::Error("Unknown collector");
    const auto revision = row.integer(0);
    Statement update(db_, "UPDATE managed_collectors SET last_seen_ms=?,applied_revision=?,apply_error=? WHERE site=?");
    update.integer(1, nowMs);
    update.integer(2, appliedRevision);
    update.string(3, applyError);
    update.string(4, site);
    update.next();
    QJsonObject state{{"site", site}, {"revision", revision}};
    if (revision != appliedRevision)
        state.insert("targets", QJsonDocument::fromJson(row.string(1).toUtf8()).array());
    return state;
}
