#pragma once

#include <QString>
#include <sqlite3.h>
#include <stdexcept>

namespace hopline_sql {
inline void check(sqlite3 *db, int result) {
    if (result != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

inline void execute(sqlite3 *db, const char *sql) {
    check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class Statement {
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        check(db, sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void reset() { check(db_, sqlite3_reset(stmt_)); }
    void integer(int i, qint64 n) { check(db_, sqlite3_bind_int64(stmt_, i, n)); }
    void number(int i, double n) { check(db_, sqlite3_bind_double(stmt_, i, n)); }
    void string(int i, const QString &s) {
        const auto u = s.toUtf8();
        check(db_, sqlite3_bind_text(stmt_, i, u.constData(), u.size(), SQLITE_TRANSIENT));
    }
    bool next() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW)
            return true;
        if (result != SQLITE_DONE)
            check(db_, result);
        return false;
    }
    qint64 integer(int i) const { return sqlite3_column_int64(stmt_, i); }
    double number(int i) const { return sqlite3_column_double(stmt_, i); }
    QString string(int i) const {
        return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(stmt_, i)));
    }

  private:
    sqlite3 *db_;
    sqlite3_stmt *stmt_ = nullptr;
};

class Transaction {
  public:
    Transaction(sqlite3 *db, bool writer) : db_(db) { execute(db_, writer ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() {
        if (active_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execute(db_, "COMMIT");
        active_ = false;
    }

  private:
    sqlite3 *db_;
    bool active_ = true;
};

} // namespace hopline_sql
