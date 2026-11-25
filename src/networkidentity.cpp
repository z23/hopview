#include "history.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QRegularExpression>

using namespace hopview_sql;

namespace {
QString encode(const QVariantMap &value) {
    return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
}

QVariantMap decode(const QString &text) {
    return QJsonDocument::fromJson(text.toUtf8()).object().toVariantMap();
}

bool validIdentity(const QVariantMap &info) {
    if ((info.size() != 7 && info.size() != 10) ||
        QHostAddress::parseSubnet(info.value("prefix").toString()).second < 0)
        return false;
    for (const auto field : {"prefix", "asn", "pop", "peer", "circuit", "source", "updated"}) {
        const auto value = info.value(field);
        if (value.typeId() != QMetaType::QString || value.toString().size() > 256)
            return false;
    }
    const auto as = info["asn"].toString();
    if (!as.isEmpty()) {
        static const QRegularExpression syntax("^AS[0-9]+([_,][0-9]+)*$");
        if (!syntax.match(as).hasMatch())
            return false;
        const auto origins = as.mid(2).split(QRegularExpression("[_,]"));
        for (const auto &n : origins) {
            bool ok;
            n.toUInt(&ok);
            if (!ok)
                return false;
        }
    }
    if (info.size() == 10) {
        if (QHostAddress::parseSubnet(info["originPrefix"].toString()).second < 0 ||
            info["originSource"].toString().isEmpty() || info["originSource"].toString().size() > 256 ||
            !QDateTime::fromString(info["originUpdated"].toString(), Qt::ISODate).isValid())
            return false;
    }
    return !info["source"].toString().isEmpty() &&
           QDateTime::fromString(info["updated"].toString(), Qt::ISODate).isValid();
}
} // namespace

bool History::importInventory(const QString &path) {
    if (!open())
        return false;
    error_.clear();
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        // Parse the input on this connection's private staging table. A full routing
        // snapshot can take much longer to validate than to publish.
        Transaction stage(db_, false);
        execute(db_, "CREATE TEMP TABLE IF NOT EXISTS incoming_prefixes(family,bits,prefix,data,origin,"
                     "PRIMARY KEY(origin,family,bits,prefix)) WITHOUT ROWID; DELETE FROM incoming_prefixes;");
        Statement insert(db_, "INSERT OR REPLACE INTO incoming_prefixes VALUES(?,?,?,?,?)");
        int line = 0, count = 0;
        while (!file.atEnd()) {
            const auto bytes = file.readLine(4097);
            const auto row = QString::fromUtf8(bytes).trimmed();
            ++line;
            if (row.isEmpty() || row.startsWith('#'))
                continue;
            auto fields = row.split('\t');
            if (fields == QStringList{"prefix", "asn", "pop", "peer", "circuit", "source"})
                continue;
            auto invalid = [&] {
                return std::runtime_error(QString("Invalid inventory at line %1. Expected prefix, ASN, PoP, "
                                                  "peer, circuit, source (tab-separated).")
                                              .arg(line)
                                              .toStdString());
            };
            const bool origin = fields.size() == 3;
            if (origin)
                fields = {fields[0] + "/" + fields[1], fields[2], {}, {}, {}, QFileInfo(path).fileName()};
            if (bytes.size() > 4096 || fields.size() != 6)
                throw invalid();
            const auto [addr, bits] = QHostAddress::parseSubnet(fields[0]);
            if (bits < 0)
                throw invalid();
            const auto prefix = addr.toString() + "/" + QString::number(bits);
            auto asn = fields[1].trimmed().toUpper();
            if (!asn.isEmpty() && !asn.startsWith("AS"))
                asn.prepend("AS");
            const QVariantMap info{{"prefix", prefix},  {"asn", asn},           {"pop", fields[2]},
                                   {"peer", fields[3]}, {"circuit", fields[4]}, {"source", fields[5]},
                                   {"updated", now}};
            if (!validIdentity(info))
                throw invalid();
            insert.reset();
            insert.integer(1, addr.protocol());
            insert.integer(2, bits);
            insert.string(3, prefix);
            insert.string(4, encode(info));
            insert.integer(5, origin);
            insert.next();
            ++count;
        }
        if (!count || file.error() != QFile::NoError)
            throw std::runtime_error("No readable inventory rows.");
        stage.commit();
        Transaction tx(db_, true);
        execute(
            db_,
            "DELETE FROM network_origins WHERE family IN (SELECT DISTINCT family FROM incoming_prefixes "
            "WHERE origin=1);"
            "INSERT INTO network_origins SELECT family,bits,prefix,data FROM incoming_prefixes WHERE "
            "origin=1;"
            "INSERT OR REPLACE INTO network_inventory SELECT family,bits,prefix,data FROM incoming_prefixes "
            "WHERE origin=0;"
            "INSERT OR IGNORE INTO inventory_lengths SELECT DISTINCT family,bits FROM incoming_prefixes;");
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantMap History::networkIdentity(qint64 id, const QString &address) {
    Statement saved(db_, "SELECT data FROM captured_identity WHERE session=? AND address=?");
    saved.integer(1, id);
    saved.string(2, address);
    if (saved.next())
        return decode(saved.string(0));
    const QHostAddress host(address);
    if (host.isNull())
        return {};
    Statement lengths(db_, "SELECT bits FROM inventory_lengths WHERE family=? ORDER BY bits DESC");
    lengths.integer(1, host.protocol());
    Statement match(db_, "SELECT data FROM network_inventory WHERE family=? AND bits=? AND prefix=?");
    Statement origin(db_, "SELECT data FROM network_origins WHERE family=? AND bits=? AND prefix=?");
    QVariantMap label, routing;
    int labelBits = -1, originBits = -1;
    while (lengths.next()) {
        const int bits = lengths.integer(0);
        const auto net =
            QHostAddress::parseSubnet(host.toString().section('%', 0, 0) + "/" + QString::number(bits));
        const auto prefix = net.first.toString() + "/" + QString::number(bits);
        auto lookup = [&](Statement &q) {
            q.reset();
            q.integer(1, host.protocol());
            q.integer(2, bits);
            q.string(3, prefix);
            return q.next() ? decode(q.string(0)) : QVariantMap{};
        };
        if (label.isEmpty()) {
            label = lookup(match);
            if (!label.isEmpty())
                labelBits = bits;
        }
        if (routing.isEmpty()) {
            routing = lookup(origin);
            if (!routing.isEmpty())
                originBits = bits;
        }
        if (!label.isEmpty() && !routing.isEmpty())
            break;
    }
    if (label.isEmpty())
        return routing;
    if (!routing.isEmpty() && (label["asn"].toString().isEmpty() || originBits > labelBits)) {
        label["asn"] = routing["asn"];
        label["originPrefix"] = routing["prefix"];
        label["originSource"] = routing["source"];
        label["originUpdated"] = routing["updated"];
    }
    return label;
}

bool History::captureIdentity(qint64 id, const QString &address, const QVariantMap &info) {
    if (!open())
        return false;
    try {
        const QHostAddress host(address);
        if (host.isNull() || !validIdentity(info) ||
            !host.isInSubnet(QHostAddress::parseSubnet(info["prefix"].toString())) ||
            (info.contains("originPrefix") && !host.isInSubnet(QHostAddress::parseSubnet(info["originPrefix"].toString()))))
            throw std::runtime_error("Invalid recorded network identity.");
        Statement put(db_, "INSERT INTO captured_identity VALUES(?,?,?)");
        put.integer(1, id);
        put.string(2, address);
        put.string(3, encode(info));
        put.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

QVariantList History::responders(qint64 id, int hop, qint64 from, qint64 through) {
    QVariantList rows;
    if (!open())
        return rows;
    error_.clear();
    try {
        Statement q(db_,
                    "SELECT address,count(*) FROM samples WHERE session=? AND hop=? AND t BETWEEN ? AND ? "
                    "AND rtt>=0 GROUP BY address ORDER BY count(*) DESC,address");
        q.integer(1, id);
        q.integer(2, hop);
        q.integer(3, from);
        q.integer(4, through);
        while (q.next())
            rows << QVariantMap{{"address", q.string(0)},
                                {"replies", q.integer(1)},
                                {"identity", networkIdentity(id, q.string(0))}};
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return rows;
}
