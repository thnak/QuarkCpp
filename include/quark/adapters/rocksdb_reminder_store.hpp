// 027-Reminders — the `RocksReminderStore` adapter: a crash-durable `ReminderStore` backed by RocksDB,
// the embedded LSM key-value store built for high write throughput. It is the reminder-seam analogue of
// `rocksdb_store.hpp` (the 012 event Store): linked ONLY when `QUARK_WITH_ROCKSDB=ON`, the engine core
// stays std-only. Models the identical `ReminderStore` seam as `InMemoryReminderStore` /
// `FileReminderStore` / `SqliteReminderStore` (reminder_service.hpp) — the SEGSTREAM service is unchanged.
//
// WHY ROCKSDB FOR REMINDERS. When reminders churn hard (millions of periodic reminders each re-armed on
// every fire ⇒ a durable upsert per fire), the LSM's sequential-write path beats a B-tree; `sync=true`
// gives the ADR-017 F1p one-fsync-per-op durability. Because RocksDB keys are compared bytewise, a
// BIG-ENDIAN due-time prefix on the key gives a NATIVE ordered on-disk due-index — a range scan returns
// reminders in `scheduled_due_ns` order, bounding the cold-open rebuild (ADR-017 residual #3).
//
// KEY LAYOUT (one DB; keys big-endian so the bytewise comparator orders reminders by due time):
//   "R" + due(8B BE) + actor(16B BE) + name_hash(8B BE)  -> value{name, period, payload}   [due-ordered]
//   "C"                                                    -> checkpoint bucket (i64 LE)     [single key]
// The due time is IN the key so a re-arm (new due) writes a new key and Deletes the old one — the row is
// addressed by ReminderKey, so `put`/`remove` first look up the current due for that key via the "I"
// index below, keeping "upsert replaces, never duplicates":
//   "I" + actor(16B BE) + name_hash(8B BE)               -> current due(8B BE)              [key -> due]
//
// NOT compiled unless the RocksDB dev headers are present (cmake/QuarkPersistenceAdapters.cmake).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/reminder_service.hpp"

namespace quark::adapters {

class RocksReminderStore {
public:
    // Open (creating if absent) a RocksDB at `path`. A failed open leaves `db_` null and every op
    // returns an error — a store that refuses writes, never a crash. The checkpoint bucket is read once
    // into RAM so `checkpoint_bucket()` is a cheap noexcept accessor.
    explicit RocksReminderStore(const std::string& path) {
        rocksdb::Options opts;
        opts.create_if_missing = true;
        rocksdb::DB* raw = nullptr;
        if (rocksdb::DB::Open(opts, path, &raw).ok()) db_.reset(raw);
        checkpoint_ = load_checkpoint();
    }
    RocksReminderStore(const RocksReminderStore&) = delete;
    RocksReminderStore& operator=(const RocksReminderStore&) = delete;

    // UPSERT a durable reminder. The due time is part of the R-key, so a re-arm must Delete the old
    // due-keyed row; we find it via the "I" (key -> current due) index. The Delete(old R) + Put(new R) +
    // Put(I) go in ONE WriteBatch written with sync=true ⇒ atomic + durable, no duplicate row survives.
    [[nodiscard]] result<void> put(const ReminderRow& row) {
        if (!db_) return fail(errc::internal, "rocksdb reminder store not open");
        const ReminderKey k = row.key();
        rocksdb::WriteBatch wb;
        std::int64_t old_due = 0;
        if (lookup_due(k, old_due)) wb.Delete(rkey(k, old_due));  // drop the stale due-keyed row
        wb.Put(rkey(k, row.scheduled_due_ns), encode_value(row));
        wb.Put(ikey(k), due_be(row.scheduled_due_ns));
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Write(wo, &wb).ok()) return fail(errc::internal, "rocksdb reminder put failed");
        return {};
    }

    // Remove a reminder (cancel / one-shot completion): Delete both the due-keyed row and its I-index
    // entry in one synced batch. Removing an absent key is a no-op success.
    [[nodiscard]] result<void> remove(ReminderKey key) {
        if (!db_) return fail(errc::internal, "rocksdb reminder store not open");
        rocksdb::WriteBatch wb;
        std::int64_t due = 0;
        if (lookup_due(key, due)) wb.Delete(rkey(key, due));
        wb.Delete(ikey(key));
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Write(wo, &wb).ok()) return fail(errc::internal, "rocksdb reminder remove failed");
        return {};
    }

    // Replay all live rows on cold open — a range scan over the "R" prefix returns them in
    // `scheduled_due_ns` order (big-endian due prefix), so the service rebuilds its due-index in order.
    [[nodiscard]] result<std::vector<ReminderRow>> load_all() {
        std::vector<ReminderRow> out;
        if (!db_) return out;
        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
        for (it->Seek("R"); it->Valid(); it->Next()) {
            const rocksdb::Slice key = it->key();
            if (key.size() == 0 || key.data()[0] != 'R') break;   // left the R-range
            if (key.size() != kRKeyLen) continue;                 // malformed
            ReminderRow r;
            // key = 'R' + due(8) + actor_type(8) + actor_key(8) + name_hash(8)
            r.scheduled_due_ns = static_cast<std::int64_t>(get_u64_be_raw(key.data() + 1));
            r.actor.type.value = get_u64_be_raw(key.data() + 9);
            r.actor.key        = get_u64_be_raw(key.data() + 17);
            if (!decode_value(it->value(), r)) continue;
            out.push_back(std::move(r));
        }
        return out;
    }

    [[nodiscard]] result<void> checkpoint(std::int64_t bucket) {
        checkpoint_ = bucket;
        if (!db_) return fail(errc::internal, "rocksdb reminder store not open");
        rocksdb::WriteOptions wo; wo.sync = true;
        if (!db_->Put(wo, "C", i64_le(bucket)).ok())
            return fail(errc::internal, "rocksdb reminder checkpoint failed");
        return {};
    }
    [[nodiscard]] std::int64_t checkpoint_bucket() const noexcept { return checkpoint_; }

    // --- diagnostics (not part of the seam) ---
    [[nodiscard]] bool healthy() const noexcept { return db_ != nullptr; }

private:
    // 'R' + due(8) + actor_type(8) + actor_key(8) + name_hash(8)
    static constexpr std::size_t kRKeyLen = 1 + 8 + 8 + 8 + 8;

    [[nodiscard]] static std::string actor_bytes(ActorId a) {
        return u64_be(a.type.value) + u64_be(a.key);
    }
    [[nodiscard]] static std::string rkey(ReminderKey k, std::int64_t due) {
        return "R" + due_be(due) + actor_bytes(k.actor) + u64_be(k.name_hash);
    }
    [[nodiscard]] static std::string ikey(ReminderKey k) {
        return "I" + actor_bytes(k.actor) + u64_be(k.name_hash);
    }

    // Current due for a key via the "I" index (so a re-arm can drop the stale due-keyed row).
    [[nodiscard]] bool lookup_due(ReminderKey k, std::int64_t& out_due) {
        std::string v;
        if (!db_ || !db_->Get(rocksdb::ReadOptions(), ikey(k), &v).ok() || v.size() < 8) return false;
        out_due = static_cast<std::int64_t>(get_u64_be_raw(v.data()));
        return true;
    }

    [[nodiscard]] std::int64_t load_checkpoint() {
        if (!db_) return -1;
        std::string v;
        if (!db_->Get(rocksdb::ReadOptions(), "C", &v).ok() || v.size() < 8) return -1;
        return get_i64_le(v.data());
    }

    // value: name(u32 len + bytes) + period(8 LE) + payload(u32 len + bytes). Endianness of the value is
    // irrelevant (only keys are scanned in order); LE chosen to match FileReminderStore's body encoding.
    [[nodiscard]] static std::string encode_value(const ReminderRow& r) {
        std::string v;
        v += u32_le(static_cast<std::uint32_t>(r.name.size()));
        v.append(r.name);
        v += i64_le(r.period_ns);
        v += u32_le(static_cast<std::uint32_t>(r.payload.size()));
        v.append(reinterpret_cast<const char*>(r.payload.data()), r.payload.size());
        return v;
    }
    [[nodiscard]] static bool decode_value(const rocksdb::Slice& s, ReminderRow& r) {
        const char* p = s.data();
        const std::size_t n = s.size();
        std::size_t o = 0;
        if (n < 4) return false;
        const std::uint32_t nl = get_u32_le(p + o); o += 4;
        if (o + nl + 8 + 4 > n) return false;
        r.name.assign(p + o, static_cast<std::size_t>(nl)); o += nl;
        r.period_ns = get_i64_le(p + o); o += 8;
        const std::uint32_t pl = get_u32_le(p + o); o += 4;
        if (o + pl > n) return false;
        r.payload.assign(reinterpret_cast<const std::byte*>(p + o),
                         reinterpret_cast<const std::byte*>(p + o) + pl);
        return true;
    }

    // Due time as a big-endian key prefix. Bias by 2^63 so NEGATIVE dues (pre-1970) still sort before
    // positive ones under the unsigned bytewise comparator (order-preserving signed->unsigned map).
    [[nodiscard]] static std::string due_be(std::int64_t due) {
        return u64_be(static_cast<std::uint64_t>(due) ^ 0x8000'0000'0000'0000ULL);
    }

    // ---- big-endian u64 (keys: lexicographic order == numeric order) --------------------------------
    [[nodiscard]] static std::string u64_be(std::uint64_t v) {
        std::string s(8, '\0');
        for (int i = 7; i >= 0; --i) { s[static_cast<std::size_t>(i)] = static_cast<char>(v & 0xFF); v >>= 8; }
        return s;
    }
    [[nodiscard]] static std::uint64_t get_u64_be_raw(const char* p) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        return v;
    }
    // ---- little-endian helpers (values: endianness irrelevant) --------------------------------------
    [[nodiscard]] static std::string i64_le(std::int64_t v) {
        auto u = static_cast<std::uint64_t>(v);
        std::string s(8, '\0');
        for (std::size_t i = 0; i < 8; ++i) { s[i] = static_cast<char>(u & 0xFF); u >>= 8; }
        return s;
    }
    [[nodiscard]] static std::int64_t get_i64_le(const char* p) {
        std::uint64_t u = 0;
        for (std::size_t i = 0; i < 8; ++i)
            u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
        return static_cast<std::int64_t>(u);
    }
    [[nodiscard]] static std::string u32_le(std::uint32_t v) {
        std::string s(4, '\0');
        for (std::size_t i = 0; i < 4; ++i) { s[i] = static_cast<char>(v & 0xFF); v >>= 8; }
        return s;
    }
    [[nodiscard]] static std::uint32_t get_u32_le(const char* p) {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
        return v;
    }

    std::unique_ptr<rocksdb::DB> db_;
    std::int64_t checkpoint_ = -1;
};

static_assert(ReminderStore<RocksReminderStore>, "RocksReminderStore must model the 027 ReminderStore seam");

}  // namespace quark::adapters
