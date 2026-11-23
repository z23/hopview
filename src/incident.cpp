#include "incident.h"
#include "history.h"
#include "probeoptions.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>

using namespace hopline_sql;
namespace {

void require(bool valid, const QString &message) {
    if (!valid)
        throw std::runtime_error(message.toStdString());
}
void writeBytes(QSaveFile &out, const QByteArray &bytes) {
    if (out.write(bytes) != bytes.size())
        throw std::runtime_error(out.errorString().toStdString());
}
void jsonLine(QSaveFile &out, const QJsonObject &object) {
    writeBytes(out, QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}
void csvRow(QSaveFile &out, const QStringList &fields) {
    QByteArray bytes;
    for (const auto &field : fields) {
        if (!bytes.isEmpty())
            bytes += ',';
        bytes += '"' + field.toUtf8().replace("\"", "\"\"") + '"';
    }
    // Keep each record the same width, including columns occupied only by session metadata.
    for (qsizetype i = fields.size(); i < 17; ++i)
        bytes += ",\"\"";
    writeBytes(out, bytes + "\r\n");
}
QString absolutePath(const QString &path) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
QString utc(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}
constexpr qint64 maxTime = 253402300799999LL; // Last millisecond of year 9999.
qint64 integer(const QJsonObject &record, const QString &key, qint64 low, qint64 high) {
    const auto value = record.value(key);
    const auto n = value.toInteger(-1);
    require(value.isDouble() && n >= low && n <= high, "Invalid incident field: " + key);
    return n;
}
} // namespace

bool History::exportIncident(qint64 id, qint64 from, qint64 through, const QString &file, bool csv) {
    if (!open())
        return false;
    error_.clear();
    try {
        require(id > 0 && from >= 0 && from <= through && through <= maxTime, "Invalid incident time range.");
        const auto source = absolutePath(path_);
        const auto output = absolutePath(file);
        for (const auto &suffix : {QString(), QString("-wal"), QString("-shm"), QString("-journal")})
            require(output != source + suffix, "Choose a file outside the active history database.");
        Transaction tx(db_, false);
        QVariantMap session;
        for (const auto &entry : sessions()) {
            if (entry.toMap().value("id").toLongLong() == id) {
                session = entry.toMap();
                break;
            }
        }
        require(!session.isEmpty(), "Recording could not be found.");
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly))
            throw std::runtime_error(out.errorString().toStdString());
        QJsonObject header = QJsonObject::fromVariantMap(session);
        for (const auto &key : {"id", "startMs", "endMs", "closed"})
            header.remove(key);
        header["kind"] = "hopline";
        header["version"] = 1;
        header["from"] = from;
        header["through"] = through;
        if (csv) {
            csvRow(out, {"kind", "time_utc", "time_ms", "hop", "address", "rtt_ms", "result", "destination",
                         "note", "target", "family", "interval_ms", "protocol", "port", "interface",
                         "from_ms", "through_ms"});
            csvRow(out, {"session",
                         {},
                         {},
                         {},
                         {},
                         {},
                         {},
                         {},
                         {},
                         session["target"].toString(),
                         session["family"].toString(),
                         session["intervalMs"].toString(),
                         session["protocol"].toString(),
                         session["port"].toString(),
                         session["interfaceName"].toString(),
                         QString::number(from),
                         QString::number(through)});
        } else {
            jsonLine(out, header);
        }
        // Seek the selected time range for each real hop using the existing primary key.
        Statement samples(db_, "SELECT s.t,s.hop,s.address,s.rtt,s.dest FROM hops h CROSS JOIN samples s "
                               "WHERE h.session=? AND s.session=h.session AND s.hop=h.hop "
                               "AND s.t BETWEEN ? AND ? ORDER BY s.t,s.hop");
        samples.integer(1, id);
        samples.integer(2, from);
        samples.integer(3, through);
        qint64 count = 0, annotations = 0;
        while (samples.next()) {
            const auto t = samples.integer(0), hop = samples.integer(1);
            const auto address = samples.string(2);
            const auto rtt = samples.number(3);
            const bool destination = samples.integer(4);
            if (csv) {
                csvRow(out, {"probe", utc(t), QString::number(t), QString::number(hop), address,
                             rtt < 0 ? QString() : QString::number(rtt, 'g', 17),
                             rtt < 0 ? "timeout" : "reply", destination ? "true" : "false"});
            } else {
                jsonLine(out, {{"kind", "probe"},
                               {"t", t},
                               {"hop", hop},
                               {"address", address},
                               {"rtt", rtt < 0 ? QJsonValue::Null : QJsonValue(rtt)},
                               {"destination", destination}});
            }
            ++count;
        }
        Statement notes(db_, "SELECT t,body FROM notes WHERE session=? AND t BETWEEN ? AND ? ORDER BY t");
        notes.integer(1, id);
        notes.integer(2, from);
        notes.integer(3, through);
        while (notes.next()) {
            const auto t = notes.integer(0);
            if (csv)
                csvRow(out, {"note", utc(t), QString::number(t), {}, {}, {}, {}, {}, notes.string(1)});
            else
                jsonLine(out, {{"kind", "note"}, {"t", t}, {"text", notes.string(1)}});
            ++annotations;
        }
        if (!csv)
            jsonLine(out, {{"kind", "end"}, {"probes", count}, {"notes", annotations}});
        tx.commit();
        if (!out.commit())
            throw std::runtime_error(out.errorString().toStdString());
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool importIncident(const QString &path, History &history, IncidentRange &range, QString &error) {
    try {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly))
            throw std::runtime_error(in.errorString().toStdString());
        auto read = [&] {
            const auto line = in.readLine(131072);
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(line, &parse);
            require(line.endsWith('\n') && doc.isObject() && parse.error == QJsonParseError::NoError,
                    "Invalid or incomplete Hopline incident record.");
            return doc.object();
        };
        const auto header = read();
        require(header["kind"] == "hopline" && integer(header, "version", 1, 1) == 1,
                "Unsupported Hopline incident format.");
        range.from = integer(header, "from", 0, maxTime);
        range.through = integer(header, "through", range.from, maxTime);
        const auto target = header["target"].toString();
        const auto family = integer(header, "family", 0, 6);
        const auto interval = integer(header, "intervalMs", 250, 60000);
        const auto options =
            ProbeOptions{header["protocol"].toString(), int(integer(header, "port", 0, 65535)),
                         header["interfaceName"].toString()};
        require(!target.trimmed().isEmpty() && target.size() <= 255 &&
                    (family == 0 || family == 4 || family == 6) && header["interfaceName"].isString() &&
                    options.interfaceName.size() <= 255 && options.valid(),
                "Invalid incident trace settings.");
        const auto id = history.createSession(target, family, interval, options.protocol, options.port,
                                              options.interfaceName);
        require(id > 0, history.error());
        QVector<Sample> batch;
        auto flush = [&] {
            if (!batch.isEmpty()) {
                if (!history.append(batch))
                    throw std::runtime_error(history.error().toStdString());
                batch.clear();
            }
        };
        qint64 probes = 0, notes = 0, previousTime = -1, previousNote = -1;
        int previousHop = 0;
        while (!in.atEnd()) {
            const auto record = read();
            const auto kind = record["kind"].toString();
            if (kind == "end") {
                require(integer(record, "probes", 0, maxTime) == probes &&
                            integer(record, "notes", 0, maxTime) == notes && in.atEnd() &&
                            in.error() == QFile::NoError,
                        "Incident record count mismatch or trailing data.");
                flush();
                return true;
            }
            const auto t = integer(record, "t", range.from, range.through);
            if (kind == "probe") {
                const int hop = integer(record, "hop", 1, 255);
                const auto rtt = record.value("rtt");
                require((rtt.isNull() ||
                         (rtt.isDouble() && rtt.toDouble() >= 0 && std::isfinite(rtt.toDouble()))) &&
                            record["destination"].isBool() && record["address"].isString() &&
                            record["address"].toString().size() <= 255 &&
                            rtt.isNull() == record["address"].toString().isEmpty() &&
                            (!rtt.isNull() || !record["destination"].toBool()),
                        "Invalid probe record.");
                require(t > previousTime || (t == previousTime && hop > previousHop),
                        "Unordered or duplicate probe.");
                batch.append({id, t, hop, record["address"].toString(), rtt.toDouble(-1),
                              record["destination"].toBool()});
                previousTime = t;
                previousHop = hop;
                ++probes;
                if (batch.size() >= 4096)
                    flush();
            } else if (kind == "note") {
                const auto note = record["text"].toString();
                require(!note.trimmed().isEmpty() && note.size() <= 2048 && t > previousNote,
                        "Invalid annotation.");
                if (!history.addNote(id, t, note))
                    throw std::runtime_error(history.error().toStdString());
                previousNote = t;
                ++notes;
            } else {
                require(false, "Unknown incident record type.");
            }
        }
        require(false, "Incident is missing its closing record.");
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
    }
    return false;
}
