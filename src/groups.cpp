#include "history.h"
#include "sqlhelpers.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace hopline_sql;

QStringList History::groupNames() {
    QStringList names;
    if (!open())
        return names;
    try {
        error_.clear();
        Statement read(db_, "SELECT name FROM comparison_groups ORDER BY name COLLATE NOCASE,name");
        while (read.next())
            names.append(read.string(0));
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    return names;
}

bool History::saveGroup(QString name, const QList<qint64> &sessions, const QList<int> &hops,
                        QVariantMap view) {
    if (!open())
        return false;
    try {
        error_.clear();
        name = name.trimmed();
        if (name.isEmpty() || name.size() > 96 || sessions.size() < 2)
            throw std::runtime_error("Choose at least two traces and a group name of up to 96 characters.");
        QJsonArray pinned;
        for (int hop : hops)
            pinned.append(hop);
        view["hops"] = pinned.toVariantList();
        Transaction tx(db_, true);
        Statement put(db_, "INSERT INTO comparison_groups(name,view_json) VALUES(?,?) "
                           "ON CONFLICT(name) DO UPDATE SET view_json=excluded.view_json");
        put.string(1, name);
        put.string(2, QString::fromUtf8(QJsonDocument::fromVariant(view).toJson(QJsonDocument::Compact)));
        put.next();
        Statement clear(db_, "DELETE FROM comparison_members WHERE name=?");
        clear.string(1, name);
        clear.next();
        Statement member(db_, "INSERT INTO comparison_members VALUES(?,?,?)");
        for (qsizetype i = 0; i < sessions.size(); ++i) {
            member.reset();
            member.string(1, name);
            member.integer(2, i);
            member.integer(3, sessions[i]);
            member.next();
        }
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::openGroup(const QString &name, QList<qint64> &sessions, QList<int> &hops, QVariantMap *view) {
    if (!open())
        return false;
    try {
        error_.clear();
        Transaction tx(db_, true);
        Statement pins(db_, "SELECT view_json FROM comparison_groups WHERE name=?");
        pins.string(1, name);
        if (!pins.next())
            throw std::runtime_error("Comparison group does not exist.");
        hops.clear();
        auto state = QJsonDocument::fromJson(pins.string(0).toUtf8()).object().toVariantMap();
        for (const auto &value : state.take("hops").toList())
            hops.append(value.toInt());
        if (view)
            *view = state;
        sessions.clear();
        Statement members(db_, "SELECT session FROM comparison_members WHERE name=? ORDER BY position");
        members.string(1, name);
        while (members.next())
            sessions.append(members.integer(0));
        Statement reopen(
            db_,
            "DELETE FROM closed_tabs WHERE session IN (SELECT session FROM comparison_members WHERE name=?)");
        reopen.string(1, name);
        reopen.next();
        tx.commit();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}

bool History::deleteGroup(const QString &name) {
    if (!open())
        return false;
    try {
        error_.clear();
        Statement del(db_, "DELETE FROM comparison_groups WHERE name=?");
        del.string(1, name);
        del.next();
        return true;
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}
