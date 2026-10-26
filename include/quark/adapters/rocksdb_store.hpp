// 012-Persistence — the `RocksStore` adapter: a crash-durable `Store` backed by RocksDB, the
// embedded LSM key-value store built for high write throughput. Linked ONLY when
// `QUARK_WITH_ROCKSDB=ON`; the engine core stays std-only. Models the identical `Store` seam as
// `InMemoryStore`/`FileStore`/`SqliteStore` — snapshot/event-log/recovery/fencing mechanism unchanged.
//
// WHY ROCKSDB. When the workload is append-heavy (event sourcing at scale, millions of actors each
// committing), an LSM tree's sequential-write path beats a B-tree; RocksDB also gives atomic
// `WriteBatch` (⇒ the 012/ADR-009 C7 all-or-none batch) and a durable WAL with `sync=true`.
//
// KEY LAYOUT (one DB; column families separate the three record kinds; keys are big-endian so RocksDB's
// bytewise comparator orders events by (actor,seq) naturally, enabling an ordered range scan):
//   default CF : "M" + actor(16B)                 -> owner token (u64 BE)      [fencing]
//   default CF : "S" + actor(16B)                 -> {fence u64, through_seq u64, bytes}
//   default CF : "E" + actor(16B) + seq(8B BE)    -> event bytes               [ordered range]
// last_seq(id) = the seq of the last "E"+actor key (a reverse seek to the actor's key upper bound).
//
// FENCING / ATOMICITY / STRICT-SEQ are enforced exactly as the reference store; the batch is one
// RocksDB `WriteBatch` written with `WriteOptions::sync = true`. NOT compiled unless the RocksDB dev
// headers are present (see cmake/QuarkPersistenceAdapters.cmake).
//
// ERROR HANDLING (review B3-B6, B11). RocksDB holds an exclusive lock on the DB directory, so this
// process is the only writer: a store-wide mutex makes every check-then-write (fence token, strict seq,
// snapshot through_seq) atomic, closing the window the old unlocked read-modify-write left open (two
// threads could be handed the same fence token). Every RocksDB Status is CHECKED: only `NotFound` means
// "no data" — an IOError/Corruption on a read is an error, so recovery never seeds default state over a
// snapshot it merely failed to read. `acquire_fence` returns a `result` and fails rather than hand out
// a token it did not durably record. `last_seq` cannot carry an error through the Store concept's
// signature and reports 0 on a read error; every WRITE path uses the checked internal read instead.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark::adapters {

class RocksStore {
public:
    // Open (creating if absent) a RocksDB at `path`. A failed open leaves `db_` null and every op
    // returns an error — a store that refuses everything, never a crash and never silent data loss.
    explicit RocksStore(const std::string& path) {
        rocksdb::Options opts;
        opts.create_if_missing = true;
        rocksdb::DB* raw = nullptr;
        const rocksdb::Status st = rocksdb::DB::Open(opts, path, &raw);
        if (st.ok()) db_.reset(raw);
    }
    RocksStore(const RocksStore&) = delete;
    RocksStore& operator=(const RocksStore&) = delete;

    [[nodiscard]] result<FenceToken> acquire_fence(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::optional<std::uint64_t> cur = owner_of(id);
        if (!cur) return fail(errc::internal, "rocksdb acquire_fence: reading the owner failed");
        const std::uint64_t next = *cur + 1;
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Put(wo, mkey(id), u64_be(next)).ok())
            return fail(errc::internal, "rocksdb acquire_fence: recording the owner failed");
        return FenceToken{next};
    }

    // The Store concept cannot carry an error here; a read failure reports 0 (writes re-check with the
    // checked read and fail, so a bogus 0 can never be built upon).
    [[nodiscard]] SeqNo last_seq(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return max_seq(id).value_or(0);
    }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        std::string v;
        const rocksdb::Status st = db_->Get(rocksdb::ReadOptions(), skey(id), &v);
        if (st.IsNotFound()) return std::optional<SnapshotRecord>{};  // genuinely none
        if (!st.ok()) return fail(errc::internal, "rocksdb load_snapshot failed");
        if (v.size() < 16) return fail(errc::internal, "rocksdb load_snapshot: malformed record");
        SnapshotRecord r;
        std::size_t p = 0;
        r.fence = FenceToken{get_u64_be(v, p)};
        r.through_seq = get_u64_be(v, p);
        r.record = std::vector<std::byte>(reinterpret_cast<const std::byte*>(v.data()) + p,
                                          reinterpret_cast<const std::byte*>(v.data()) + v.size());
        return std::optional<SnapshotRecord>{std::move(r)};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::optional<std::uint64_t> owner = owner_of(id);
        if (!owner) return fail(errc::internal, "rocksdb save_snapshot: reading the owner failed");
        if (is_unfenced(tok) || tok < FenceToken{*owner}) return fenced_error();
        const std::optional<SeqNo> tail = max_seq(id);
        if (!tail) return fail(errc::internal, "rocksdb save_snapshot: reading the tail failed");
        if (snap.through_seq > *tail)
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
        // Review B11: never roll the recovery point back behind a newer checkpoint.
        std::string cur;
        const rocksdb::Status gs = db_->Get(rocksdb::ReadOptions(), skey(id), &cur);
        if (!gs.ok() && !gs.IsNotFound())
            return fail(errc::internal, "rocksdb save_snapshot: reading the stored snapshot failed");
        if (gs.ok()) {
            if (cur.size() < 16)
                return fail(errc::internal, "rocksdb save_snapshot: malformed stored snapshot");
            std::size_t q = 8;  // skip the stored fence
            if (snap.through_seq < get_u64_be(cur, q))
                return fail(errc::internal, "stale snapshot: through_seq older than the stored snapshot");
        }
        std::string v = u64_be(tok.value) + u64_be(snap.through_seq);
        v.append(reinterpret_cast<const char*>(snap.record.data()), snap.record.size());
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Put(wo, skey(id), v).ok()) return fail(errc::internal, "rocksdb save_snapshot failed");
        return {};
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        EventRecord one{seq, std::vector<std::byte>(bytes.begin(), bytes.end())};
        return append_batch(id, tok, std::span<const EventRecord>(&one, 1));
    }

    // ATOMIC batch (012/ADR-009 C7): validate fence + strict monotonicity (under `mu_`, so the check
    // cannot go stale), then one RocksDB WriteBatch written with sync=true — RocksDB applies the whole
    // batch atomically & durably, or not at all.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        const std::optional<std::uint64_t> owner = owner_of(id);
        if (!owner) return fail(errc::internal, "rocksdb append: reading the owner failed");
        if (is_unfenced(tok) || tok < FenceToken{*owner}) return fenced_error();
        const std::optional<SeqNo> tail = max_seq(id);
        if (!tail) return fail(errc::internal, "rocksdb append: reading the tail failed");
        SeqNo prev = *tail;
        for (const EventRecord& r : batch) {
            if (r.seq <= prev) return fail(errc::internal, "commit sequence not strictly increasing");
            prev = r.seq;
        }
        rocksdb::WriteBatch wb;
        for (const EventRecord& r : batch)
            wb.Put(ekey(id, r.seq),
                   rocksdb::Slice(reinterpret_cast<const char*>(r.record.data()), r.record.size()));
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Write(wo, &wb).ok()) return fail(errc::internal, "rocksdb batch write failed");
        return {};
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) {
        std::lock_guard<std::mutex> g(mu_);
        if (!db_) return unhealthy();
        EventCursor cur;
        const std::string lo = ekey(id, from);
        const std::string prefix = "E" + actor_bytes(id);
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek(lo); it->Valid(); it->Next()) {
            const rocksdb::Slice k = it->key();
            if (k.size() < prefix.size() || std::memcmp(k.data(), prefix.data(), prefix.size()) != 0)
                break;  // left this actor's E-range
            const SeqNo seq = get_u64_be_raw(k.data() + prefix.size());
            const rocksdb::Slice val = it->value();
            cur.entries.push_back(EventRecord{
                seq, std::vector<std::byte>(reinterpret_cast<const std::byte*>(val.data()),
                                            reinterpret_cast<const std::byte*>(val.data()) + val.size())});
        }
        // An iterator that stopped on an error (not on the end of the range) would otherwise look like
        // a short log; surface it.
        if (!it->status().ok()) return fail(errc::internal, "rocksdb read_log failed");
        return cur;
    }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] FenceToken current_owner(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return FenceToken{owner_of(id).value_or(0)};
    }
    [[nodiscard]] bool healthy() const noexcept { return db_ != nullptr; }

private:
    [[nodiscard]] static std::unexpected<error> unhealthy() {
        return fail(errc::internal, "RocksStore: database is not open (failed open)");
    }

    // 16-byte big-endian actor identity (type||key) so the bytewise comparator groups an actor's rows.
    [[nodiscard]] static std::string actor_bytes(ActorId id) {
        return u64_be(id.type.value) + u64_be(id.key);
    }
    [[nodiscard]] static std::string mkey(ActorId id) { return "M" + actor_bytes(id); }
    [[nodiscard]] static std::string skey(ActorId id) { return "S" + actor_bytes(id); }
    [[nodiscard]] static std::string ekey(ActorId id, SeqNo seq) {
        return "E" + actor_bytes(id) + u64_be(seq);
    }

    // The owner token, 0 if the actor has none, nullopt on a READ ERROR (never confused with 0).
    [[nodiscard]] std::optional<std::uint64_t> owner_of(ActorId id) {
        if (!db_) return std::nullopt;
        std::string v;
        const rocksdb::Status st = db_->Get(rocksdb::ReadOptions(), mkey(id), &v);
        if (st.IsNotFound()) return std::uint64_t{0};
        if (!st.ok() || v.size() < 8) return std::nullopt;
        std::size_t p = 0;
        return get_u64_be(v, p);
    }
    // last_seq: reverse-seek to the largest E-key for this actor (keys are ordered by BE seq).
    // nullopt on a read error, 0 if the actor has no events.
    [[nodiscard]] std::optional<SeqNo> max_seq(ActorId id) {
        if (!db_) return std::nullopt;
        const std::string prefix = "E" + actor_bytes(id);
        std::string upper = prefix;
        upper.append(8, static_cast<char>(0xFF));  // just past this actor's largest seq key
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        it->SeekForPrev(upper);
        if (!it->Valid()) {
            if (!it->status().ok()) return std::nullopt;
            return SeqNo{0};
        }
        const rocksdb::Slice k = it->key();
        if (k.size() != prefix.size() + 8 ||
            std::memcmp(k.data(), prefix.data(), prefix.size()) != 0)
            return SeqNo{0};  // no event rows for this actor
        return get_u64_be_raw(k.data() + prefix.size());
    }

    // ---- big-endian u64 helpers (so lexicographic key order == numeric order) --------------------
    [[nodiscard]] static std::string u64_be(std::uint64_t v) {
        std::string s(8, '\0');
        for (int i = 7; i >= 0; --i) { s[static_cast<std::size_t>(i)] = static_cast<char>(v & 0xFF); v >>= 8; }
        return s;
    }
    [[nodiscard]] static std::uint64_t get_u64_be(const std::string& s, std::size_t& p) {
        std::uint64_t v = get_u64_be_raw(s.data() + p);
        p += 8;
        return v;
    }
    [[nodiscard]] static std::uint64_t get_u64_be_raw(const char* p) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        return v;
    }

    std::mutex mu_;
    std::unique_ptr<rocksdb::DB> db_;
};

static_assert(Store<RocksStore>, "RocksStore must model the 012 Store seam");

}  // namespace quark::adapters
