// 027-Reminders — the `SqliteReminderStore` adapter: a crash-durable `ReminderStore` backed by SQLite
// (embedded, single-file, ACID). It is the reminder-seam analogue of `sqlite_store.hpp` (the 012 event
// Store): same vetted single-file dependency, linked ONLY when `QUARK_WITH_SQLITE=ON`, the engine core
// stays std-only. It models the identical `ReminderStore` seam as `InMemoryReminderStore` /
// `FileReminderStore` (reminder_service.hpp), so the SEGSTREAM service + due-index/token-bucket
// MECHANISM is unchanged — only the backing store differs (PersistenceAdapters.md).
//
// WHY SQLITE FOR REMINDERS. The reminder access pattern is keyed `(actor, name)` with a full-set replay
// on cold open and a due-time-ordered scan — exactly a table with a primary key and a secondary index.
// SQLite gives durable upsert/delete (PRAGMA synchronous=FULL ⇒ one fsync per op, the ADR-017 F1p
// contract), a NATIVE ordered on-disk due-index (`rem_due`) so `load_all()` returns rows already sorted
// by `scheduled_due_ns` (bounding the cold-open due-index rebuild — ADR-017 residual #3), and a single
// portable, inspectable file.
//
// SCHEMA (one DB file for the whole reminder set; actor identity stored as its two u64 halves):
//   reminders(actor_type, actor_key, name_hash, name, scheduled_due_ns, period_ns, payload,
//             PRIMARY KEY(actor_type, actor_key, name_hash))         -- the UPSERT target = ReminderKey
//   rem_due  INDEX ON reminders(scheduled_due_ns)                    -- ordered due-index
//   rem_meta(k INTEGER PRIMARY KEY, v INTEGER)                       -- k=0 -> checkpoint bucket
//
// u64 identity fields are stored bit-exact in SQLite's signed INTEGER column (reinterpret round-trips).
// NOT compiled unless the SQLite dev headers are present (cmake/QuarkPersistenceAdapters.cmake).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

namespace quark::adapters {

class SqliteReminderStore {
public:
    // Open (creating if absent) the SQLite database at `path`. Enables WAL journaling + synchronous
    // durability and creates the schema. Throws-free: a failed open leaves `db_` null and every op
    // returns an error (a store that refuses writes, never a crash). The checkpoint bucket is read once
    // into RAM so `checkpoint_bucket()` is a cheap noexcept accessor (as the reference stores are).
    explicit SqliteReminderStore(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            db_ = nullptr;
            return;
        }
        exec("PRAGMA journal_mode=WAL;");     // concurrent readers + durable, fast commits
        exec("PRAGMA synchronous=FULL;");     // fsync on commit — the ADR-017 F1p durability contract
        exec("CREATE TABLE IF NOT EXISTS reminders(actor_type INTEGER NOT NULL, actor_key INTEGER NOT NULL,"
             " name_hash INTEGER NOT NULL, name TEXT NOT NULL, scheduled_due_ns INTEGER NOT NULL,"
             " period_ns INTEGER NOT NULL, payload BLOB,"
             " PRIMARY KEY(actor_type, actor_key, name_hash));");
        exec("CREATE INDEX IF NOT EXISTS rem_due ON reminders(scheduled_due_ns);");  // ordered due-index
        exec("CREATE TABLE IF NOT EXISTS rem_meta(k INTEGER PRIMARY KEY, v INTEGER NOT NULL);");
        checkpoint_ = load_checkpoint();
    }
    ~SqliteReminderStore() { if (db_) sqlite3_close(db_); }
    SqliteReminderStore(const SqliteReminderStore&) = delete;
    SqliteReminderStore& operator=(const SqliteReminderStore&) = delete;

    // UPSERT a durable reminder, keyed by ReminderKey `(actor, name_hash)`. One implicit transaction ⇒
    // one fsync (synchronous=FULL). Re-registering the same key replaces the row, so a handler that
    // re-arms every run does not accumulate duplicates (ADR-017).
    [[nodiscard]] result<void> put(const ReminderRow& row) {
        const ReminderKey k = row.key();
        Stmt s(db_, "INSERT INTO reminders(actor_type,actor_key,name_hash,name,scheduled_due_ns,period_ns,payload)"
                    " VALUES(?1,?2,?3,?4,?5,?6,?7)"
                    " ON CONFLICT(actor_type,actor_key,name_hash) DO UPDATE SET"
                    " name=?4, scheduled_due_ns=?5, period_ns=?6, payload=?7;");
        s.bind_u64(1, row.actor.type.value);
        s.bind_u64(2, row.actor.key);
        s.bind_u64(3, k.name_hash);
        s.bind_text(4, row.name);
        s.bind_i64(5, row.scheduled_due_ns);
        s.bind_i64(6, row.period_ns);
        s.bind_blob(7, row.payload);
        if (!s.step_done()) return fail(errc::internal, "sqlite reminder put failed");
        return {};
    }

    // Remove a reminder (cancel / one-shot completion). Deleting an absent key is a no-op success.
    [[nodiscard]] result<void> remove(ReminderKey key) {
        Stmt s(db_, "DELETE FROM reminders WHERE actor_type=?1 AND actor_key=?2 AND name_hash=?3;");
        s.bind_u64(1, key.actor.type.value);
        s.bind_u64(2, key.actor.key);
        s.bind_u64(3, key.name_hash);
        if (!s.step_done()) return fail(errc::internal, "sqlite reminder remove failed");
        return {};
    }

    // Replay all live rows on cold open — returned pre-sorted by `scheduled_due_ns` (the `rem_due`
    // index), so the service rebuilds its ordered due-index without re-sorting.
    [[nodiscard]] result<std::vector<ReminderRow>> load_all() {
        std::vector<ReminderRow> out;
        Stmt s(db_, "SELECT actor_type, actor_key, name, scheduled_due_ns, period_ns, payload"
                    " FROM reminders ORDER BY scheduled_due_ns ASC;");
        while (s.step_row()) {
            ReminderRow r;
            r.actor.type.value = static_cast<std::uint64_t>(s.col_i64(0));
            r.actor.key        = static_cast<std::uint64_t>(s.col_i64(1));
            r.name             = s.col_text(2);
            r.scheduled_due_ns = static_cast<std::int64_t>(s.col_i64(3));
            r.period_ns        = static_cast<std::int64_t>(s.col_i64(4));
            r.payload          = s.col_blob(5);
            out.push_back(std::move(r));
        }
        return out;
    }

    // Durable "resolved through here" marker (rebuild-skip optimization). Single row upsert.
    [[nodiscard]] result<void> checkpoint(std::int64_t bucket) {
        checkpoint_ = bucket;
        Stmt s(db_, "INSERT INTO rem_meta(k,v) VALUES(0,?1) ON CONFLICT(k) DO UPDATE SET v=?1;");
        s.bind_i64(1, bucket);
        if (!s.step_done()) return fail(errc::internal, "sqlite reminder checkpoint failed");
        return {};
    }
    [[nodiscard]] std::int64_t checkpoint_bucket() const noexcept { return checkpoint_; }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] bool healthy() const noexcept { return db_ != nullptr; }

private:
    // RAII prepared-statement wrapper — prepare in ctor, finalize in dtor; bind/step/column helpers.
    struct Stmt {
        sqlite3_stmt* st = nullptr;
        Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &st, nullptr); }
        ~Stmt() { if (st) sqlite3_finalize(st); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        void bind_text(int i, const std::string& v) {
            sqlite3_bind_text(st, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }
        void bind_i64(int i, std::int64_t v) { sqlite3_bind_int64(st, i, static_cast<sqlite3_int64>(v)); }
        // A u64 stored bit-exact in SQLite's signed 64-bit column (round-trips via reinterpret).
        void bind_u64(int i, std::uint64_t v) { sqlite3_bind_int64(st, i, static_cast<sqlite3_int64>(v)); }
        void bind_blob(int i, const std::vector<std::byte>& v) {
            sqlite3_bind_blob(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }
        [[nodiscard]] bool step_row() { return st && sqlite3_step(st) == SQLITE_ROW; }
        [[nodiscard]] bool step_done() { return st && sqlite3_step(st) == SQLITE_DONE; }
        [[nodiscard]] sqlite3_int64 col_i64(int c) { return sqlite3_column_int64(st, c); }
        [[nodiscard]] std::string col_text(int c) {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
            const int n = sqlite3_column_bytes(st, c);
            return p ? std::string(p, static_cast<std::size_t>(n)) : std::string{};
        }
        [[nodiscard]] std::vector<std::byte> col_blob(int c) {
            const auto* p = static_cast<const std::byte*>(sqlite3_column_blob(st, c));
            const int n = sqlite3_column_bytes(st, c);
            return p ? std::vector<std::byte>(p, p + n) : std::vector<std::byte>{};
        }
    };

    void exec(const char* sql) { if (db_) sqlite3_exec(db_, sql, nullptr, nullptr, nullptr); }

    [[nodiscard]] std::int64_t load_checkpoint() {
        Stmt s(db_, "SELECT v FROM rem_meta WHERE k=0;");
        return s.step_row() ? static_cast<std::int64_t>(s.col_i64(0)) : -1;
    }

    sqlite3* db_ = nullptr;
    std::int64_t checkpoint_ = -1;
};

static_assert(ReminderStore<SqliteReminderStore>, "SqliteReminderStore must model the 027 ReminderStore seam");

}  // namespace quark::adapters
