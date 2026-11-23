#include "incident.h"
#include "history.h"
#include "probeoptions.h"
#include "sqlhelpers.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cmath>

using namespace hopline_sql;
namespace {
constexpr qint64 maxTime = 253402300799999LL; // Last millisecond of year 9999.

void require(bool valid, const QString &message) {
    if (!valid)
        throw std::runtime_error(message.toStdString());
}
void checkHistory(bool ok, History &history) {
    if (!ok)
        throw std::runtime_error(history.error().toStdString());
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
qint64 integer(const QJsonObject &record, const QString &key, qint64 low, qint64 high) {
    const auto value = record.value(key);
    const auto n = value.toDouble();
    require(value.isDouble() && n >= low && n <= high && n == std::floor(n),
            "Invalid incident field: " + key);
    return static_cast<qint64>(n);
}
QString stringField(const QJsonObject &record, const QString &key, int maxLength) {
    const auto value = record.value(key);
    require(value.isString() && value.toString().size() <= maxLength &&
                !value.toString().contains(QChar::Null),
            "Invalid incident field: " + key);
    return value.toString();
}
QJsonArray pins(const QList<int> &hops) {
    QJsonArray array;
    for (int hop : hops)
        array.append(hop);
    return array;
}
QJsonObject readRecord(QFile &in, int limit) {
    const auto line = in.readLine(limit);
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(line, &parse);
    require(line.endsWith('\n') && doc.isObject() && parse.error == QJsonParseError::NoError,
            "Invalid or incomplete Hopline incident record.");
    return doc.object();
}
struct Counts {
    qint64 probes = 0, notes = 0, diagnostics = 0;
};

void writeTrace(sqlite3 *db, History &history, QSaveFile &out, const QVariantMap &info, int index,
                qint64 from, qint64 through, bool csv, Counts &counts) {
    const auto id = info.value("id").toLongLong();
    const QStringList settings{info["target"].toString(),
                               info["family"].toString(),
                               info["intervalMs"].toString(),
                               info["protocol"].toString(),
                               info["port"].toString(),
                               info["interfaceName"].toString(),
                               QString::number(from),
                               QString::number(through),
                               QString::number(index),
                               info["location"].toString(),
                               info["resolvedAddress"].toString(),
                               info["clockOffsetMs"].toString(),
                               info["clockErrorMs"].toString(),
                               info["manualCorrectionMs"].toString()};
    auto emitRecord = [&](QJsonObject record, QStringList fields) {
        if (csv) {
            fields.resize(9);
            csvRow(out, fields + settings +
                            QStringList{record.isEmpty() ? QString()
                                                         : QString::fromUtf8(QJsonDocument(record).toJson(
                                                               QJsonDocument::Compact))});
        } else {
            record["trace"] = index;
            jsonLine(out, record);
        }
    };
    if (csv)
        emitRecord({}, {"session"});
    Statement samples(db, "SELECT s.t,s.hop,s.address,s.rtt,s.dest FROM hops h CROSS JOIN samples s "
                          "WHERE h.session=? AND s.session=h.session AND s.hop=h.hop "
                          "AND s.t BETWEEN ? AND ? ORDER BY s.t,s.hop");
    samples.integer(1, id);
    samples.integer(2, from);
    samples.integer(3, through);
    while (samples.next()) {
        const auto t = samples.integer(0), hop = samples.integer(1);
        const auto address = samples.string(2);
        const auto rtt = samples.number(3);
        const bool destination = samples.integer(4);
        if (csv)
            emitRecord({}, {"probe", utc(t), QString::number(t), QString::number(hop), address,
                            rtt < 0 ? QString() : QString::number(rtt, 'g', 17),
                            rtt < 0 ? "timeout" : "reply", destination ? "true" : "false"});
        else
            emitRecord({{"kind", "probe"},
                        {"t", t},
                        {"hop", hop},
                        {"address", address},
                        {"rtt", rtt < 0 ? QJsonValue::Null : QJsonValue(rtt)},
                        {"destination", destination}},
                       {});
        ++counts.probes;
    }
    Statement notes(db, "SELECT t,body FROM notes WHERE session=? AND t BETWEEN ? AND ? ORDER BY t");
    notes.integer(1, id);
    notes.integer(2, from);
    notes.integer(3, through);
    while (notes.next()) {
        const auto t = notes.integer(0);
        emitRecord(csv ? QJsonObject{} : QJsonObject{{"kind", "note"}, {"t", t}, {"text", notes.string(1)}},
                   {"note", utc(t), QString::number(t), {}, {}, {}, {}, {}, notes.string(1)});
        ++counts.notes;
    }
    checkHistory(history.eachDiagnostic(id, from, through,
                                        [&](const QVariantMap &value) {
                                            const auto record = QJsonObject::fromVariantMap(value);
                                            const auto t = value["t"].toLongLong();
                                            if (csv)
                                                emitRecord(record, {value["kind"].toString(), utc(t),
                                                                    QString::number(t)});
                                            else
                                                emitRecord(record, {});
                                            ++counts.diagnostics;
                                        }),
                 history);
}
struct Cursor {
    qint64 time = -1, note = -1;
    int hop = 0;
    QMap<QString, qint64> diagnostics;
};

qint64 addTrace(History &history, QJsonObject meta, const QString &fallback, qint64 correction, bool modern) {
    const auto target = stringField(meta, "target", 255);
    const auto family = integer(meta, "family", 0, 6);
    const auto interval = integer(meta, "intervalMs", 250, 60000);
    const ProbeOptions options{stringField(meta, "protocol", 8), int(integer(meta, "port", 0, 65535)),
                               stringField(meta, "interfaceName", 255)};
    require(!target.trimmed().isEmpty() && (family == 0 || family == 4 || family == 6) && options.valid(),
            "Invalid incident trace settings.");
    auto location = modern ? stringField(meta, "location", 255) : QString{};
    if (location.isEmpty())
        location = fallback;
    const auto address = modern ? stringField(meta, "resolvedAddress", 255) : QString{};
    const auto offset = modern ? integer(meta, "clockOffsetMs", -maxTime, maxTime) : 0;
    const auto uncertainty = modern ? integer(meta, "clockErrorMs", -1, maxTime) : -1;
    const auto received = modern ? integer(meta, "lastReceivedMs", 0, maxTime) : 0;
    const auto applied = modern ? integer(meta, "manualCorrectionMs", -maxTime, maxTime) : 0;
    require(std::abs(applied + correction) <= maxTime, "Invalid cumulative clock adjustment.");
    const auto id = history.createSession(target, family, interval, options.protocol, options.port,
                                          options.interfaceName);
    checkHistory(id > 0, history);
    checkHistory(history.setSessionOrigin(id, location, address, offset, uncertainty, received), history);
    checkHistory(history.markImported(id, applied + correction), history);
    return id;
}

void parseIncident(QFile &in, History &stage, IncidentRange &range, const QString &site, qint64 correction) {
    const auto header = readRecord(in, 16 * 1024 * 1024);
    require(header["kind"] == "hopline", "Unsupported Hopline incident format.");
    const int version = integer(header, "version", 1, 2);
    const auto from = integer(header, "from", 0, maxTime);
    const auto through = integer(header, "through", from, maxTime);
    require(correction >= -maxTime && correction <= maxTime && from + correction >= 0 &&
                through + correction <= maxTime,
            "Clock correction puts the incident outside the supported time range.");
    require(site.size() <= 255, "Site name is too long.");
    range.from = from + correction;
    range.through = through + correction;
    QJsonArray traces;
    if (version == 1)
        traces.append(header);
    else {
        traces = header["traces"].toArray();
        require(!traces.isEmpty() && header["hops"].isArray(),
                "Incident has no primary trace or hop selection.");
        range.pinnedHops.clear();
        for (auto value : header["hops"].toArray()) {
            const int n = value.toInt(-1);
            require(n >= 0 && n <= 255 && !range.pinnedHops.contains(n), "Invalid pinned hop.");
            range.pinnedHops.append(n);
        }
    }
    for (const auto &entry : traces)
        range.sessions.append(addTrace(stage, entry.toObject(), site, correction, version == 2));
    QVector<Cursor> cursors(traces.size());
    QVector<Sample> batch;
    Counts counts;
    auto flush = [&] {
        if (!batch.isEmpty()) {
            checkHistory(stage.append(batch), stage);
            batch.clear();
        }
    };
    while (!in.atEnd()) {
        const auto record = readRecord(in, 131072);
        const auto kind = record.value("kind").toString();
        if (kind == "end") {
            require(integer(record, "probes", 0, maxTime) == counts.probes &&
                        integer(record, "notes", 0, maxTime) == counts.notes &&
                        (version == 1 || integer(record, "diagnostics", 0, maxTime) == counts.diagnostics) &&
                        in.atEnd() && in.error() == QFile::NoError,
                    "Incident record count mismatch or trailing data.");
            flush();
            return;
        }
        const auto t = integer(record, "t", from, through) + correction;
        const auto i = version == 1 ? 0 : integer(record, "trace", 0, range.sessions.size() - 1);
        const auto id = range.sessions[i];
        auto &cursor = cursors[i];
        if (kind == "probe") {
            const int hop = integer(record, "hop", 1, 255);
            const auto address = stringField(record, "address", 255);
            const auto rtt = record.value("rtt");
            require(
                (rtt.isNull() || (rtt.isDouble() && rtt.toDouble() >= 0 && std::isfinite(rtt.toDouble()))) &&
                    record["destination"].isBool() && rtt.isNull() == address.isEmpty() &&
                    (!rtt.isNull() || !record["destination"].toBool()),
                "Invalid probe record.");
            require(t > cursor.time || (t == cursor.time && hop > cursor.hop),
                    "Unordered or duplicate probe.");
            cursor.time = t;
            cursor.hop = hop;
            batch.append({id, t, hop, address, rtt.toDouble(-1), record["destination"].toBool()});
            ++counts.probes;
            if (batch.size() >= 4096)
                flush();
        } else if (kind == "note") {
            const auto note = stringField(record, "text", 2048);
            require(!note.trimmed().isEmpty() && t > cursor.note, "Invalid annotation.");
            checkHistory(stage.addNote(id, t, note), stage);
            cursor.note = t;
            ++counts.notes;
        } else if (version == 2 && (kind == "interface" || kind == "service")) {
            require(t > cursor.diagnostics.value(kind, -1), "Unordered or duplicate diagnostics record.");
            auto data = record.toVariantMap();
            data.remove("trace");
            data["t"] = t;
            checkHistory(stage.appendDiagnostic(id, data), stage);
            cursor.diagnostics[kind] = t;
            ++counts.diagnostics;
        } else
            require(false, "Unknown incident record type.");
    }
    throw std::runtime_error("Incident is missing its closing record.");
}
} // namespace

bool History::exportIncident(qint64 id, qint64 from, qint64 through, const QString &file, bool csv,
                             QList<qint64> comparisons, QList<int> pinnedHops) {
    if (!open())
        return false;
    error_.clear();
    try {
        require(id > 0 && from >= 0 && from <= through && through <= maxTime, "Invalid incident range.");
        const auto source = absolutePath(path_), output = absolutePath(file);
        for (const auto &suffix : {QString(), QString("-wal"), QString("-shm"), QString("-journal")})
            require(output != source + suffix, "Choose a file outside the active history database.");
        Transaction tx(db_, false);
        QHash<qint64, QVariantMap> available;
        for (auto entry : sessions())
            available.insert(entry.toMap().value("id").toLongLong(), entry.toMap());
        comparisons.prepend(id);
        require(comparisons.size() == QSet<qint64>(comparisons.begin(), comparisons.end()).size(),
                "Duplicate incident trace.");
        QJsonArray traces;
        for (const auto member : comparisons) {
            require(available.contains(member), "Recording could not be found.");
            QJsonObject meta;
            const auto &session = available[member];
            for (const auto &key :
                 {"target", "family", "intervalMs", "protocol", "port", "interfaceName", "location",
                  "resolvedAddress", "clockOffsetMs", "clockErrorMs", "lastReceivedMs", "manualCorrectionMs"})
                meta.insert(key, QJsonValue::fromVariant(session.value(key)));
            traces.append(meta);
        }
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly))
            throw std::runtime_error(out.errorString().toStdString());
        if (csv)
            csvRow(out, {"kind",
                         "time_utc",
                         "time_ms",
                         "hop",
                         "address",
                         "rtt_ms",
                         "result",
                         "destination",
                         "note",
                         "target",
                         "family",
                         "interval_ms",
                         "protocol",
                         "port",
                         "interface",
                         "from_ms",
                         "through_ms",
                         "trace",
                         "location",
                         "resolved_address",
                         "clock_offset_ms",
                         "clock_error_ms",
                         "manual_correction_ms",
                         "diagnostic_json"});
        else
            jsonLine(out, {{"kind", "hopline"},
                           {"version", 2},
                           {"from", from},
                           {"through", through},
                           {"traces", traces},
                           {"hops", pins(pinnedHops)}});
        Counts counts;
        for (qsizetype i = 0; i < comparisons.size(); ++i)
            writeTrace(db_, *this, out, available[comparisons[i]], i, from, through, csv, counts);
        if (!csv)
            jsonLine(out, {{"kind", "end"},
                           {"probes", counts.probes},
                           {"notes", counts.notes},
                           {"diagnostics", counts.diagnostics}});
        tx.commit();
        if (!out.commit())
            throw std::runtime_error(out.errorString().toStdString());
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::markImported(qint64 id, qint64 correction) {
    if (!open())
        return false;
    try {
        Statement add(db_, "INSERT OR REPLACE INTO imported_sessions VALUES(?,?)");
        add.integer(1, id);
        add.integer(2, correction);
        add.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::adoptIncident(const QString &database, QList<qint64> &ids, qint64 from, qint64 through) {
    if (!open())
        return false;
    error_.clear();
    try {
        // Only our validated, private staging database is attached here.
        {
            Statement attach(db_, "ATTACH DATABASE ? AS evidence");
            attach.string(1, database);
            attach.next();
        }
        struct Detach {
            sqlite3 *db;
            ~Detach() { sqlite3_exec(db, "DETACH DATABASE evidence", nullptr, nullptr, nullptr); }
        } detach{db_};
        Transaction tx(db_, true);
        for (auto &id : ids) {
            Statement add(
                db_,
                "INSERT INTO sessions(target,family,interval_ms,created_ms,first_ms,last_ms) "
                "SELECT target,family,interval_ms,created_ms,coalesce(first_ms,?1),coalesce(last_ms,?2) FROM "
                "evidence.sessions WHERE id=?3");
            add.integer(1, from);
            add.integer(2, through);
            add.integer(3, id);
            add.next();
            require(sqlite3_changes(db_) == 1, "Missing staged incident trace.");
            const auto imported = sqlite3_last_insert_rowid(db_);
            // Reuse the validated rows and rollups. Never transfer local recorder intent or receiver
            // bindings.
            for (const char *table : {"session_probe", "session_origin", "imported_sessions", "notes", "hops",
                                      "samples", "bins", "diagnostic_records", "diagnostic_bins"}) {
                const QByteArray name(table);
                const QByteArray schema = "PRAGMA main.table_info(" + name + ")";
                Statement columns(db_, schema.constData());
                QByteArrayList fields;
                while (columns.next()) {
                    const auto column = columns.string(1).toUtf8();
                    if (column != "session")
                        fields.append(column);
                }
                require(!fields.isEmpty(), "Missing local evidence table.");
                const auto cols = fields.join(',');
                const auto sql = "INSERT INTO main." + name + "(session," + cols + ") SELECT ?," + cols +
                                 " FROM evidence." + name + " WHERE session=?";
                Statement copy(db_, sql.constData());
                copy.integer(1, imported);
                copy.integer(2, id);
                copy.next();
            }
            id = imported;
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool importIncident(const QString &path, History &history, IncidentRange &range, QString &error,
                    const QString &site, qint64 correctionMs) {
    try {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly))
            throw std::runtime_error(in.errorString().toStdString());
        QTemporaryDir scratch;
        require(scratch.isValid(), scratch.errorString());
        const auto staged = scratch.filePath("evidence.sqlite");
        IncidentRange selected;
        {
            History stage(staged);
            parseIncident(in, stage, selected, site.trimmed(), correctionMs);
        }
        checkHistory(history.adoptIncident(staged, selected.sessions, selected.from, selected.through),
                     history);
        range = selected;
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = QString::fromUtf8(e.what());
        return false;
    }
}
