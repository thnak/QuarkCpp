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
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
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
    // returns an error — a store that refuses writes, never a crash.
    explicit RocksStore(const std::string& path) {
        rocksdb::Options opts;
        opts.create_if_missing = true;
        rocksdb::DB* raw = nullptr;
        const rocksdb::Status st = rocksdb::DB::Open(opts, path, &raw);
        if (st.ok()) db_.reset(raw);
    }
    RocksStore(const RocksStore&) = delete;
    RocksStore& operator=(const RocksStore&) = delete;

    [[nodiscard]] FenceToken acquire_fence(ActorId id) {
        // NOTE: read-modify-write of the owner is serialized by the caller (activation registration is
        // single-threaded per actor); a multi-writer deployment would use a RocksDB merge-operator or
        // an optimistic transaction. Here: read current, write current+1, sync.
        const std::uint64_t next = owner_of(id) + 1;
        std::string v = u64_be(next);
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_ || !db_->Put(wo, mkey(id), v).ok()) return FenceToken{next - 1};
        return FenceToken{next};
    }

    [[nodiscard]] SeqNo last_seq(ActorId id) { return max_seq(id); }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) {
        std::string v;
        if (!db_ || !db_->Get(rocksdb::ReadOptions(), skey(id), &v).ok())
            return std::optional<SnapshotRecord>{};  // none
        SnapshotRecord r;
        std::size_t p = 0;
        r.fence = FenceToken{get_u64_be(v, p)};
        r.through_seq = get_u64_be(v, p);
        r.record = std::vector<std::byte>(reinterpret_cast<const std::byte*>(v.data()) + p,
                                          reinterpret_cast<const std::byte*>(v.data()) + v.size());
        return std::optional<SnapshotRecord>{std::move(r)};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        if (tok < FenceToken{owner_of(id)}) return fenced_error();
        if (snap.through_seq > max_seq(id))
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
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

    // ATOMIC batch (012/ADR-009 C7): validate fence + strict monotonicity, then one RocksDB WriteBatch
    // written with sync=true — RocksDB applies the whole batch atomically & durably, or not at all.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        if (tok < FenceToken{owner_of(id)}) return fenced_error();
        SeqNo prev = max_seq(id);
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
        EventCursor cur;
        if (!db_) return cur;
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
        return cur;
    }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] FenceToken current_owner(ActorId id) { return FenceToken{owner_of(id)}; }
    [[nodiscard]] bool healthy() const noexcept { return db_ != nullptr; }

private:
    // 16-byte big-endian actor identity (type||key) so the bytewise comparator groups an actor's rows.
    [[nodiscard]] static std::string actor_bytes(ActorId id) {
        return u64_be(id.type.value) + u64_be(id.key);
    }
    [[nodiscard]] static std::string mkey(ActorId id) { return "M" + actor_bytes(id); }
    [[nodiscard]] static std::string skey(ActorId id) { return "S" + actor_bytes(id); }
    [[nodiscard]] static std::string ekey(ActorId id, SeqNo seq) {
        return "E" + actor_bytes(id) + u64_be(seq);
    }

    [[nodiscard]] std::uint64_t owner_of(ActorId id) {
        std::string v;
        if (!db_ || !db_->Get(rocksdb::ReadOptions(), mkey(id), &v).ok() || v.size() < 8) return 0;
        std::size_t p = 0;
        return get_u64_be(v, p);
    }
    // last_seq: reverse-seek to the largest E-key for this actor (keys are ordered by BE seq).
    [[nodiscard]] SeqNo max_seq(ActorId id) {
        if (!db_) return 0;
        const std::string prefix = "E" + actor_bytes(id);
        std::string upper = prefix;
        upper.append(8, static_cast<char>(0xFF));  // just past this actor's largest seq key
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        it->SeekForPrev(upper);
        if (!it->Valid()) return 0;
        const rocksdb::Slice k = it->key();
        if (k.size() != prefix.size() + 8 ||
            std::memcmp(k.data(), prefix.data(), prefix.size()) != 0)
            return 0;  // no event rows for this actor
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

    std::unique_ptr<rocksdb::DB> db_;
};

static_assert(Store<RocksStore>, "RocksStore must model the 012 Store seam");

}  // namespace quark::adapters
