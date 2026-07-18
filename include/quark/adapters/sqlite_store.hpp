// 012-Persistence — the `SqliteStore` adapter: a crash-durable `Store` backed by SQLite (embedded,
// single-file, ACID). SQLite is a vetted single-file library (the same "one audited dependency"
// exception the RFC grants real crypto), linked ONLY when `QUARK_WITH_SQLITE=ON`; the engine core
// stays std-only. It models the identical `Store` seam as `InMemoryStore`/`FileStore`, so the
// snapshot/event-log/recovery/fencing MECHANISM is unchanged — only the backing store differs.
//
// WHY SQLITE. It gives ACID transactions (⇒ the 012/ADR-009 C7 all-or-none batch is one SQL
// transaction), durable `fsync` on commit (PRAGMA synchronous), queryability/inspection, and a
// single portable file — a good default "real database" when RocksDB's write throughput isn't needed.
//
// SCHEMA (one DB file for the whole store; actors keyed by "type-key" text):
//   meta     (actor TEXT PRIMARY KEY, owner INTEGER NOT NULL)              -- the fencing owner token
//   snapshots(actor TEXT PRIMARY KEY, fence INTEGER, through_seq INTEGER, bytes BLOB)
//   events   (actor TEXT, seq INTEGER, bytes BLOB, PRIMARY KEY(actor, seq)) -- last_seq = MAX(seq)
//
// FENCING / ATOMICITY / STRICT-SEQ are enforced exactly as the reference store, inside SQL
// transactions so a crash mid-commit leaves the log unchanged. NOT compiled unless the SQLite dev
// headers are present (see cmake/QuarkPersistenceAdapters.cmake). Error text from SQLite is folded
// into `errc::internal`; a stale-token write returns `errc::unavailable` (the fence taxonomy slot).
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark::adapters {

class SqliteStore {
public:
    // Open (creating if absent) the SQLite database at `path`. Enables WAL journaling + synchronous
    // durability and creates the schema. Throws-free: a failed open leaves `db_` null and every op
    // returns an error (the caller sees a store that refuses writes, never a crash).
    explicit SqliteStore(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            db_ = nullptr;
            return;
        }
        exec("PRAGMA journal_mode=WAL;");     // concurrent readers + durable, fast commits
        exec("PRAGMA synchronous=FULL;");     // fsync on commit — the 012 Sync durability contract
        exec("CREATE TABLE IF NOT EXISTS meta(actor TEXT PRIMARY KEY, owner INTEGER NOT NULL);");
        exec("CREATE TABLE IF NOT EXISTS snapshots(actor TEXT PRIMARY KEY, fence INTEGER,"
             " through_seq INTEGER, bytes BLOB);");
        exec("CREATE TABLE IF NOT EXISTS events(actor TEXT, seq INTEGER, bytes BLOB,"
             " PRIMARY KEY(actor, seq));");
    }
    ~SqliteStore() { if (db_) sqlite3_close(db_); }
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    [[nodiscard]] FenceToken acquire_fence(ActorId id) {
        const std::string a = key_of(id);
        exec("BEGIN IMMEDIATE;");
        const std::uint64_t next = owner_of(a) + 1;
        Stmt s(db_, "INSERT INTO meta(actor,owner) VALUES(?1,?2)"
                    " ON CONFLICT(actor) DO UPDATE SET owner=?2;");
        s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(next));
        const bool ok = s.step_done();
        exec(ok ? "COMMIT;" : "ROLLBACK;");
        return FenceToken{ok ? next : next - 1};
    }

    [[nodiscard]] SeqNo last_seq(ActorId id) { return max_seq(key_of(id)); }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) {
        Stmt s(db_, "SELECT fence, through_seq, bytes FROM snapshots WHERE actor=?1;");
        s.bind_text(1, key_of(id));
        if (!s.step_row()) return std::optional<SnapshotRecord>{};  // none
        SnapshotRecord r;
        r.fence = FenceToken{static_cast<std::uint64_t>(s.col_i64(0))};
        r.through_seq = static_cast<SeqNo>(s.col_i64(1));
        r.record = s.col_blob(2);
        return std::optional<SnapshotRecord>{std::move(r)};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        const std::string a = key_of(id);
        if (tok < FenceToken{owner_of(a)}) return fenced_error();
        if (snap.through_seq > max_seq(a))
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
        Stmt s(db_, "INSERT INTO snapshots(actor,fence,through_seq,bytes) VALUES(?1,?2,?3,?4)"
                    " ON CONFLICT(actor) DO UPDATE SET fence=?2, through_seq=?3, bytes=?4;");
        s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(tok.value));
        s.bind_i64(3, static_cast<sqlite3_int64>(snap.through_seq)); s.bind_blob(4, snap.record);
        if (!s.step_done()) return fail(errc::internal, "sqlite save_snapshot failed");
        return {};
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        EventRecord one{seq, std::vector<std::byte>(bytes.begin(), bytes.end())};
        return append_batch(id, tok, std::span<const EventRecord>(&one, 1));
    }

    // ATOMIC batch (012/ADR-009 C7): validate fence + strict monotonicity, INSERT all rows inside ONE
    // SQL transaction — SQLite commits it atomically & durably, or ROLLBACK leaves the log unchanged.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        const std::string a = key_of(id);
        if (tok < FenceToken{owner_of(a)}) return fenced_error();
        SeqNo prev = max_seq(a);
        for (const EventRecord& r : batch) {
            if (r.seq <= prev) return fail(errc::internal, "commit sequence not strictly increasing");
            prev = r.seq;
        }
        exec("BEGIN IMMEDIATE;");
        for (const EventRecord& r : batch) {
            Stmt s(db_, "INSERT INTO events(actor,seq,bytes) VALUES(?1,?2,?3);");
            s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(r.seq)); s.bind_blob(3, r.record);
            if (!s.step_done()) { exec("ROLLBACK;"); return fail(errc::internal, "sqlite append failed"); }
        }
        exec("COMMIT;");
        return {};
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) {
        EventCursor cur;
        Stmt s(db_, "SELECT seq, bytes FROM events WHERE actor=?1 AND seq>=?2 ORDER BY seq ASC;");
        s.bind_text(1, key_of(id)); s.bind_i64(2, static_cast<sqlite3_int64>(from));
        while (s.step_row())
            cur.entries.push_back(EventRecord{static_cast<SeqNo>(s.col_i64(0)), s.col_blob(1)});
        return cur;
    }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] FenceToken current_owner(ActorId id) { return FenceToken{owner_of(key_of(id))}; }
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
        void bind_i64(int i, sqlite3_int64 v) { sqlite3_bind_int64(st, i, v); }
        void bind_blob(int i, const std::vector<std::byte>& v) {
            sqlite3_bind_blob(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }
        [[nodiscard]] bool step_row() { return st && sqlite3_step(st) == SQLITE_ROW; }
        [[nodiscard]] bool step_done() { return st && sqlite3_step(st) == SQLITE_DONE; }
        [[nodiscard]] sqlite3_int64 col_i64(int c) { return sqlite3_column_int64(st, c); }
        [[nodiscard]] std::vector<std::byte> col_blob(int c) {
            const auto* p = static_cast<const std::byte*>(sqlite3_column_blob(st, c));
            const int n = sqlite3_column_bytes(st, c);
            return p ? std::vector<std::byte>(p, p + n) : std::vector<std::byte>{};
        }
    };

    void exec(const char* sql) { if (db_) sqlite3_exec(db_, sql, nullptr, nullptr, nullptr); }

    [[nodiscard]] std::uint64_t owner_of(const std::string& a) {
        Stmt s(db_, "SELECT owner FROM meta WHERE actor=?1;");
        s.bind_text(1, a);
        return s.step_row() ? static_cast<std::uint64_t>(s.col_i64(0)) : 0;
    }
    [[nodiscard]] SeqNo max_seq(const std::string& a) {
        Stmt s(db_, "SELECT COALESCE(MAX(seq),0) FROM events WHERE actor=?1;");
        s.bind_text(1, a);
        return s.step_row() ? static_cast<SeqNo>(s.col_i64(0)) : 0;
    }

    [[nodiscard]] static std::string key_of(ActorId id) {
        char b[40];
        std::snprintf(b, sizeof(b), "%016llx-%016llx",
                      static_cast<unsigned long long>(id.type.value),
                      static_cast<unsigned long long>(id.key));
        return b;
    }

    sqlite3* db_ = nullptr;
};

static_assert(Store<SqliteStore>, "SqliteStore must model the 012 Store seam");

}  // namespace quark::adapters
