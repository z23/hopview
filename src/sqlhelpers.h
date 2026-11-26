#pragma once

#include <QString>
#include <sqlite3.h>
#include <stdexcept>

namespace hopview_sql {
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
    // sqlite3_reset repeats the error of a failed step, which its caller already reported.
    // A statement that outlives one call must not carry that error into the next.
    void recover() { sqlite3_reset(stmt_); }
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
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    void commit() {
        execute(db_, "COMMIT");
        active_ = false;
    }

  private:
    sqlite3 *db_;
    bool active_ = true;
};

// Opens a transaction, or a savepoint inside one that is already open. A nested operation
// that fails still undoes its own writes, so the outer transaction cannot commit half of it.
class OptionalTransaction {
  public:
    OptionalTransaction(sqlite3 *db, bool writer) : db_(db), owned_(sqlite3_get_autocommit(db) != 0) {
        execute(db_, !owned_ ? "SAVEPOINT hopview_nested" : writer ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~OptionalTransaction() {
        if (active_)
            sqlite3_exec(db_, owned_ ? "ROLLBACK" : "ROLLBACK TO hopview_nested; RELEASE hopview_nested",
                         nullptr, nullptr, nullptr);
    }
    OptionalTransaction(const OptionalTransaction &) = delete;
    OptionalTransaction &operator=(const OptionalTransaction &) = delete;
    void commit() {
        execute(db_, owned_ ? "COMMIT" : "RELEASE hopview_nested");
        active_ = false;
    }

  private:
    sqlite3 *db_;
    bool owned_;
    bool active_ = true;
};

} // namespace hopview_sql
