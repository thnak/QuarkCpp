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
//
// ERROR HANDLING (review B3-B6, B11). One connection is shared, so every public method takes `mu_`
// (BEGIN/COMMIT from two threads would otherwise interleave on it). Every SQLite call's return code is
// CHECKED: a failed BEGIN/COMMIT/step is an error to the caller, never a silent success (a failed
// COMMIT used to return ok, a failed BEGIN ran inserts in autocommit => a partially committed batch).
// Every validation (fence token, strict seq, snapshot through_seq) happens INSIDE the BEGIN IMMEDIATE
// transaction that performs the write, so a stale writer cannot slip in between check and commit.
// A READ error is an error, never "no data": recovery must not seed default state over a snapshot it
// merely failed to read. `acquire_fence` returns a `result` and fails rather than hand out a token it
// did not durably record. `last_seq` cannot carry an error through the Store concept's signature and
// reports 0 on a read error; every WRITE path uses the checked internal read and fails instead.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
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
    // durability and creates the schema. Throws-free: a failed open (or failed schema creation) leaves
    // the store unhealthy and every op returns an error (the caller sees a store that refuses
    // everything, never a crash and never silent data loss).
    explicit SqliteStore(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            return;
        }
        const bool ok =
            exec("PRAGMA journal_mode=WAL;") &&  // concurrent readers + durable, fast commits
            exec("PRAGMA synchronous=FULL;") &&  // fsync on commit — the 012 Sync durability contract
            exec("CREATE TABLE IF NOT EXISTS meta(actor TEXT PRIMARY KEY, owner INTEGER NOT NULL);") &&
            exec("CREATE TABLE IF NOT EXISTS snapshots(actor TEXT PRIMARY KEY, fence INTEGER,"
                 " through_seq INTEGER, bytes BLOB);") &&
            exec("CREATE TABLE IF NOT EXISTS events(actor TEXT, seq INTEGER, bytes BLOB,"
                 " PRIMARY KEY(actor, seq));");
        if (!ok) {  // schema not usable: refuse everything instead of limping on
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }
    ~SqliteStore() { if (db_) sqlite3_close(db_); }
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    [[nodiscard]] result<FenceToken> acquire_fence(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::string a = key_of(id);
        Tx tx(*this);
        if (!tx.begin()) return fail(errc::internal, "sqlite acquire_fence: BEGIN failed");
        const std::optional<std::uint64_t> cur = owner_of(a);
        if (!cur) return fail(errc::internal, "sqlite acquire_fence: reading the owner failed");
        const std::uint64_t next = *cur + 1;
        Stmt s(db_, "INSERT INTO meta(actor,owner) VALUES(?1,?2)"
                    " ON CONFLICT(actor) DO UPDATE SET owner=?2;");
        s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(next));
        if (s.step() != Step::Done)
            return fail(errc::internal, "sqlite acquire_fence: recording the owner failed");
        if (!tx.commit()) return fail(errc::internal, "sqlite acquire_fence: COMMIT failed");
        return FenceToken{next};
    }

    // The Store concept cannot carry an error here; a read failure reports 0 (writes re-check with the
    // checked read and fail, so a bogus 0 can never be built upon).
    [[nodiscard]] SeqNo last_seq(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return max_seq(key_of(id)).value_or(0);
    }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        Stmt s(db_, "SELECT fence, through_seq, bytes FROM snapshots WHERE actor=?1;");
        s.bind_text(1, key_of(id));
        switch (s.step()) {
            case Step::Done: return std::optional<SnapshotRecord>{};  // genuinely none
            case Step::Error: return fail(errc::internal, "sqlite load_snapshot failed");
            case Step::Row: break;
        }
        SnapshotRecord r;
        r.fence = FenceToken{static_cast<std::uint64_t>(s.col_i64(0))};
        r.through_seq = static_cast<SeqNo>(s.col_i64(1));
        r.record = s.col_blob(2);
        return std::optional<SnapshotRecord>{std::move(r)};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::string a = key_of(id);
        Tx tx(*this);
        if (!tx.begin()) return fail(errc::internal, "sqlite save_snapshot: BEGIN failed");
        const std::optional<std::uint64_t> owner = owner_of(a);
        if (!owner) return fail(errc::internal, "sqlite save_snapshot: reading the owner failed");
        if (is_unfenced(tok) || tok < FenceToken{*owner}) return fenced_error();
        const std::optional<SeqNo> tail = max_seq(a);
        if (!tail) return fail(errc::internal, "sqlite save_snapshot: reading the tail failed");
        if (snap.through_seq > *tail)
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
        std::optional<SeqNo> stored;
        if (!snapshot_through(a, stored))
            return fail(errc::internal, "sqlite save_snapshot: reading the stored snapshot failed");
        if (stored && snap.through_seq < *stored)  // review B11
            return fail(errc::internal, "stale snapshot: through_seq older than the stored snapshot");
        Stmt s(db_, "INSERT INTO snapshots(actor,fence,through_seq,bytes) VALUES(?1,?2,?3,?4)"
                    " ON CONFLICT(actor) DO UPDATE SET fence=?2, through_seq=?3, bytes=?4;");
        s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(tok.value));
        s.bind_i64(3, static_cast<sqlite3_int64>(snap.through_seq)); s.bind_blob(4, snap.record);
        if (s.step() != Step::Done) return fail(errc::internal, "sqlite save_snapshot failed");
        if (!tx.commit()) return fail(errc::internal, "sqlite save_snapshot: COMMIT failed");
        return {};
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        EventRecord one{seq, std::vector<std::byte>(bytes.begin(), bytes.end())};
        return append_batch(id, tok, std::span<const EventRecord>(&one, 1));
    }

    // ATOMIC batch (012/ADR-009 C7): fence + strict monotonicity are validated and ALL rows inserted
    // inside ONE `BEGIN IMMEDIATE` transaction (which also takes the write lock, so the checks cannot
    // go stale). SQLite commits it atomically & durably, or the scope guard ROLLBACKs and the log is
    // unchanged. A failed BEGIN or COMMIT is an error, never a success.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::string a = key_of(id);
        Tx tx(*this);
        if (!tx.begin()) return fail(errc::internal, "sqlite append: BEGIN failed");
        const std::optional<std::uint64_t> owner = owner_of(a);
        if (!owner) return fail(errc::internal, "sqlite append: reading the owner failed");
        if (is_unfenced(tok) || tok < FenceToken{*owner}) return fenced_error();
        const std::optional<SeqNo> tail = max_seq(a);
        if (!tail) return fail(errc::internal, "sqlite append: reading the tail failed");
        SeqNo prev = *tail;
        for (const EventRecord& r : batch) {
            if (r.seq <= prev) return fail(errc::internal, "commit sequence not strictly increasing");
            prev = r.seq;
        }
        for (const EventRecord& r : batch) {
            Stmt s(db_, "INSERT INTO events(actor,seq,bytes) VALUES(?1,?2,?3);");
            s.bind_text(1, a); s.bind_i64(2, static_cast<sqlite3_int64>(r.seq)); s.bind_blob(3, r.record);
            if (s.step() != Step::Done) return fail(errc::internal, "sqlite append failed");
        }
        if (!tx.commit()) return fail(errc::internal, "sqlite append: COMMIT failed");
        return {};
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        EventCursor cur;
        Stmt s(db_, "SELECT seq, bytes FROM events WHERE actor=?1 AND seq>=?2 ORDER BY seq ASC;");
        s.bind_text(1, key_of(id)); s.bind_i64(2, static_cast<sqlite3_int64>(from));
        for (;;) {
            const Step st = s.step();
            if (st == Step::Done) break;
            if (st == Step::Error) return fail(errc::internal, "sqlite read_log failed");  // never a short log
            cur.entries.push_back(EventRecord{static_cast<SeqNo>(s.col_i64(0)), s.col_blob(1)});
        }
        return cur;
    }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] FenceToken current_owner(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return FenceToken{owner_of(key_of(id)).value_or(0)};
    }
    [[nodiscard]] bool healthy() const noexcept { return db_ != nullptr; }

private:
    enum class Step { Row, Done, Error };

    // RAII prepared-statement wrapper — prepare in ctor, finalize in dtor; bind/step/column helpers.
    // A statement that failed to prepare has `st == nullptr`; step() then reports Error.
    struct Stmt {
        sqlite3_stmt* st = nullptr;
        Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &st, nullptr); }
        ~Stmt() { if (st) sqlite3_finalize(st); }
        Stmt(const Stmt&) = delete;
        Stmt& operator=(const Stmt&) = delete;
        void bind_text(int i, const std::string& v) {
            if (st) sqlite3_bind_text(st, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }
        void bind_i64(int i, sqlite3_int64 v) { if (st) sqlite3_bind_int64(st, i, v); }
        void bind_blob(int i, const std::vector<std::byte>& v) {
            if (st) sqlite3_bind_blob(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }
        [[nodiscard]] Step step() {
            if (!st) return Step::Error;
            const int rc = sqlite3_step(st);
            return rc == SQLITE_ROW ? Step::Row : rc == SQLITE_DONE ? Step::Done : Step::Error;
        }
        [[nodiscard]] sqlite3_int64 col_i64(int c) { return sqlite3_column_int64(st, c); }
        [[nodiscard]] std::vector<std::byte> col_blob(int c) {
            const auto* p = static_cast<const std::byte*>(sqlite3_column_blob(st, c));
            const int n = sqlite3_column_bytes(st, c);
            return p ? std::vector<std::byte>(p, p + n) : std::vector<std::byte>{};
        }
    };

    // One write transaction. begin() is checked; the destructor ROLLBACKs unless commit() succeeded,
    // so every early `return` above leaves the database unchanged.
    struct Tx {
        explicit Tx(SqliteStore& s) : s_(s) {}
        ~Tx() { if (open_) (void)s_.exec("ROLLBACK;"); }
        Tx(const Tx&) = delete;
        Tx& operator=(const Tx&) = delete;
        [[nodiscard]] bool begin() { return open_ = s_.exec("BEGIN IMMEDIATE;"); }
        [[nodiscard]] bool commit() {
            if (!s_.exec("COMMIT;")) return false;  // still open: the destructor rolls back
            open_ = false;
            return true;
        }
        SqliteStore& s_;
        bool open_ = false;
    };

    [[nodiscard]] bool exec(const char* sql) {
        return db_ != nullptr && sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    [[nodiscard]] static std::unexpected<error> unhealthy() {
        return fail(errc::internal, "SqliteStore: database is not open (failed open or schema creation)");
    }

    // The owner token, 0 if the actor has none, nullopt on a READ ERROR (never confused with 0).
    [[nodiscard]] std::optional<std::uint64_t> owner_of(const std::string& a) {
        if (!db_) return std::nullopt;
        Stmt s(db_, "SELECT owner FROM meta WHERE actor=?1;");
        s.bind_text(1, a);
        switch (s.step()) {
            case Step::Row: return static_cast<std::uint64_t>(s.col_i64(0));
            case Step::Done: return std::uint64_t{0};
            default: return std::nullopt;
        }
    }
    [[nodiscard]] std::optional<SeqNo> max_seq(const std::string& a) {
        if (!db_) return std::nullopt;
        Stmt s(db_, "SELECT COALESCE(MAX(seq),0) FROM events WHERE actor=?1;");
        s.bind_text(1, a);
        if (s.step() != Step::Row) return std::nullopt;
        return static_cast<SeqNo>(s.col_i64(0));
    }
    // false on a read error; `out` is the stored snapshot's through_seq, or nullopt if none.
    [[nodiscard]] bool snapshot_through(const std::string& a, std::optional<SeqNo>& out) {
        out.reset();
        if (!db_) return false;
        Stmt s(db_, "SELECT through_seq FROM snapshots WHERE actor=?1;");
        s.bind_text(1, a);
        switch (s.step()) {
            case Step::Row: out = static_cast<SeqNo>(s.col_i64(0)); return true;
            case Step::Done: return true;
            default: return false;
        }
    }

    [[nodiscard]] static std::string key_of(ActorId id) {
        char b[40];
        std::snprintf(b, sizeof(b), "%016llx-%016llx",
                      static_cast<unsigned long long>(id.type.value),
                      static_cast<unsigned long long>(id.key));
        return b;
    }

    std::mutex mu_;
    sqlite3* db_ = nullptr;
};

static_assert(Store<SqliteStore>, "SqliteStore must model the 012 Store seam");

}  // namespace quark::adapters
