// Implements 027-Reminders — the DURABLE, WALL-CLOCK, AT-LEAST-ONCE scheduled-wake-up subsystem,
// built as the SEGSTREAM design that won the design→debate→prove gate (decisions/ADR-017). A
// reminder is a durable record {actor, name, scheduled_due (civil time), period, payload} that
// survives process restart AND actor passivation and fires as a normal `tell` on the actor's own
// lane — the thing an 011 in-memory monotonic timer deliberately is NOT (011 §"Timer vs reminder").
//
// THE LOAD-BEARING PROPERTY (ADR-017, proven): a mass-due wave — 10⁶ reminders all due at one civil
// instant ("21:00") — FLATTENS into a paced, bounded fire load instead of melting the node. SEGSTREAM
// achieves this by modelling the due segment as a STREAM drained under a token-bucket FIRE RATE:
// `spread` is the drain rate, so the peak dispatch is `fire_rate` EXACTLY (not N), and the per-tick
// scan is O(due-now) not O(total) via an ordered due-index. Precision is the deliberate trade —
// reminders are bucket-granular (± drain smear), timers stay ms-precise.
//
// DURABILITY / AT-LEAST-ONCE (ADR-017 S2p — completion-gated). The durable ROW is the truth. A fire
// is confirmed — the row advanced (periodic) or removed (one-shot) durably — only AFTER the fire
// callback has run. A crash BEFORE that confirmation leaves the old row, so on reopen the reminder is
// due again and RE-FIRES with the SAME dedup key `(name, scheduled_due)` (at-least-once). Handlers
// MUST be idempotent on that key (017). Advancing the checkpoint/row BEFORE the fire (the eager
// anti-pattern) loses committed reminders — ADR-017 measured 709 lost; completion-gated loses 0/42.
//
// CLOCK DOMAIN (ADR-017 C1). Reminders read `pal::wall_now()` (CLOCK_REALTIME) — civil time that
// FOLLOWS NTP/DST steps — NOT the monotonic BootClock deadlines use. The service is driven by an
// explicit `tick(WallInstant now)` so tests/benches are deterministic (no wall-clock sleeps), exactly
// as the 011 timer sample drives the wheel with `advance_ticks`. Production calls `tick(pal::wall_now())`.
//
// SCOPE. This header owns the durable ReminderStore seam + its reference (InMemory) and durable
// (File) adapters + the SEGSTREAM service. Firing invokes a user callback that models the `tell` on
// the lane (the local delivery vehicle is 011); wiring it into the live engine's reactivation path
// (ADR-008 cold activation) is the engine-integration seam. std-only C++23; SQLite/RocksDB reminder
// backends would plug the same seam behind QUARK_WITH_* (PersistenceAdapters.md), out of scope here.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "pal/pal.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"

namespace quark {

// ============================================================================================
// Value types
// ============================================================================================

// A civil/absolute instant (nanoseconds since the Unix epoch), read from `pal::wall_now()`. A plain
// integer so it is trivially durable and comparable to civil time; NOT a monotonic BootClock instant
// (ADR-017 C1 — the compiler keeps the two domains apart at the pal seam).
struct WallInstant {
    std::int64_t ns = 0;  // since 1970-01-01T00:00:00Z
    friend constexpr bool operator==(WallInstant, WallInstant) = default;
    friend constexpr bool operator<(WallInstant a, WallInstant b) noexcept { return a.ns < b.ns; }
    friend constexpr bool operator<=(WallInstant a, WallInstant b) noexcept { return a.ns <= b.ns; }
};

[[nodiscard]] inline WallInstant wall_from_pal(pal::wall_clock::time_point tp) noexcept {
    return WallInstant{tp.time_since_epoch().count()};
}
[[nodiscard]] inline WallInstant wall_now() noexcept { return wall_from_pal(pal::wall_now()); }

// splitmix64 — the deterministic mixer 026 uses for placement; here it derives a stable per-reminder
// DRAIN RANK so the smear is identical across runs/compilers/restart (ADR-017 C2/C4p).
[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E37'79B9'7F4A'7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBULL;
    return x ^ (x >> 31);
}

// FNV-1a over the reminder name → a stable 64-bit id. `(actor, name_hash)` is the reminder KEY
// (upsert target); `(name_hash, scheduled_due)` is the idempotency DEDUP key handed to the handler.
[[nodiscard]] inline std::uint64_t reminder_name_hash(std::string_view name) noexcept {
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    for (char c : name) { h ^= static_cast<std::uint8_t>(c); h *= 0x0000'0100'0000'01B3ULL; }
    return h;
}

// The reminder key: which named reminder on which actor. Re-registering the same key is an UPSERT.
struct ReminderKey {
    ActorId actor{};
    std::uint64_t name_hash = 0;
    friend constexpr bool operator==(const ReminderKey&, const ReminderKey&) = default;
    [[nodiscard]] std::uint64_t hash() const noexcept {
        return splitmix64(actor.hash() ^ (name_hash * 0x9E37'79B9'7F4A'7C15ULL));
    }
};

// A durable reminder row — the unit the ReminderStore persists and the service rebuilds its
// due-index from on cold open. `payload` is 016 canonical-tagged described-message bytes (opaque to
// the store, decoded into the actor's message on fire).
struct ReminderRow {
    ActorId actor{};
    std::string name;
    std::int64_t scheduled_due_ns = 0;  // civil instant of THIS occurrence
    std::int64_t period_ns = 0;         // 0 ⇒ one-shot; >0 ⇒ periodic (next = scheduled + period)
    std::vector<std::byte> payload;

    [[nodiscard]] ReminderKey key() const noexcept {
        return ReminderKey{actor, reminder_name_hash(name)};
    }
};

// What the actor's fire handler receives. Carries the deterministic dedup key so a late/duplicate
// fire (at-least-once) is idempotent (017): dedup on `(name_hash, scheduled_due_ns)`.
struct FireEvent {
    ActorId actor{};
    std::string_view name;
    std::uint64_t name_hash = 0;
    std::int64_t scheduled_due_ns = 0;  // dedup key part
    std::int64_t fired_at_ns = 0;       // the wall instant it actually fired (≥ scheduled + smear)
    std::span<const std::byte> payload;
};

}  // namespace quark

template <>
struct std::hash<quark::ReminderKey> {
    std::size_t operator()(const quark::ReminderKey& k) const noexcept { return k.hash(); }
};

namespace quark {

// ============================================================================================
// The durable seam — a `ReminderStore` concept, parallel to the 012 event `Store` (ADR-017 /
// PersistenceAdapters.md). Same backend families, DIFFERENT access pattern: the event Store is keyed
// `(actor, seq)` for replay; reminders are keyed `(actor, name)` with a full-set replay on open to
// rebuild the due-index. The row is the durable truth; completion mutates it.
// ============================================================================================
template <class S>
concept ReminderStore = requires(S& s, const ReminderRow& row, ReminderKey key, std::int64_t bucket) {
    { s.put(row) } -> std::same_as<result<void>>;                     // UPSERT (durable) a reminder
    { s.remove(key) } -> std::same_as<result<void>>;                  // cancel / one-shot completion
    { s.load_all() } -> std::same_as<result<std::vector<ReminderRow>>>;// replay live rows on cold open
    { s.checkpoint(bucket) } -> std::same_as<result<void>>;           // durable "resolved through here"
    { s.checkpoint_bucket() } -> std::same_as<std::int64_t>;          // last checkpoint (rebuild skip)
};

// --------------------------------------------------------------------------------------------
// Reference adapter — RAM only. Exercises every seam property (upsert/remove/replay/checkpoint) but
// is NOT durable across process exit. The logic tests + the mass-due benchmark run on this (fast);
// the DURABILITY proof runs on FileReminderStore (below).
// --------------------------------------------------------------------------------------------
class InMemoryReminderStore {
public:
    [[nodiscard]] result<void> put(const ReminderRow& row) { rows_[row.key()] = row; return {}; }
    [[nodiscard]] result<void> remove(ReminderKey key) { rows_.erase(key); return {}; }
    [[nodiscard]] result<std::vector<ReminderRow>> load_all() const {
        std::vector<ReminderRow> out;
        out.reserve(rows_.size());
        for (const auto& [k, r] : rows_) out.push_back(r);
        return out;
    }
    [[nodiscard]] result<void> checkpoint(std::int64_t bucket) { checkpoint_ = bucket; return {}; }
    [[nodiscard]] std::int64_t checkpoint_bucket() const noexcept { return checkpoint_; }
    [[nodiscard]] std::size_t live_rows() const noexcept { return rows_.size(); }

private:
    std::unordered_map<ReminderKey, ReminderRow> rows_;
    std::int64_t checkpoint_ = -1;
};
static_assert(ReminderStore<InMemoryReminderStore>, "InMemoryReminderStore must model ReminderStore");

// --------------------------------------------------------------------------------------------
// Durable adapter — an append-only, CRC32-framed WAL (`.qrem`) + `fdatasync`, replayed on open, with
// torn-trailing-record truncation (a crash mid-write is detected and dropped, never half-read). This
// is the reminder analogue of `file_store.hpp`; it makes the ADR-017 crash-zero-loss proof REAL:
// destroy the service + store, reopen the store from the same path, and no committed reminder is
// lost. `compact()` rewrites only live rows, bounding cold-open rebuild to O(live) (residual risk #3).
// --------------------------------------------------------------------------------------------
class FileReminderStore {
public:
    enum class Kind : std::uint8_t { kPut = 1, kRemove = 2, kCheckpoint = 3 };

    explicit FileReminderStore(std::string path) : path_(std::move(path)) { replay(); open_append(); }
    ~FileReminderStore() { if (fd_ >= 0) ::close(fd_); }
    FileReminderStore(const FileReminderStore&) = delete;
    FileReminderStore& operator=(const FileReminderStore&) = delete;

    [[nodiscard]] result<void> put(const ReminderRow& row) {
        rows_[row.key()] = row;
        std::vector<std::byte> body = encode_put(row);
        return durable_append(Kind::kPut, body);
    }
    [[nodiscard]] result<void> remove(ReminderKey key) {
        rows_.erase(key);
        std::vector<std::byte> body = encode_key(key);
        return durable_append(Kind::kRemove, body);
    }
    [[nodiscard]] result<std::vector<ReminderRow>> load_all() const {
        std::vector<ReminderRow> out;
        out.reserve(rows_.size());
        for (const auto& [k, r] : rows_) out.push_back(r);
        return out;
    }
    [[nodiscard]] result<void> checkpoint(std::int64_t bucket) {
        checkpoint_ = bucket;
        std::array<std::byte, 8> b{};
        put_i64(b.data(), bucket);
        return durable_append(Kind::kCheckpoint, {b.data(), b.size()});
    }
    [[nodiscard]] std::int64_t checkpoint_bucket() const noexcept { return checkpoint_; }
    [[nodiscard]] std::size_t live_rows() const noexcept { return rows_.size(); }

    // Rewrite the log to only the live rows + the current checkpoint — bounds rebuild to O(live).
    // (ADR-017 residual #3: 10⁴ re-arms of one reminder ⇒ 1 live row, not 10⁴ log records.)
    [[nodiscard]] result<void> compact() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        const std::string tmp = path_ + ".compact";
        int t = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (t < 0) return fail(errc::internal, "reminder compact: open tmp failed");
        for (const auto& [k, r] : rows_)
            if (auto e = frame_write(t, Kind::kPut, encode_put(r)); !e) { ::close(t); return e; }
        { std::array<std::byte, 8> b{}; put_i64(b.data(), checkpoint_);
          if (auto e = frame_write(t, Kind::kCheckpoint, {b.data(), b.size()}); !e) { ::close(t); return e; } }
        sync_data(t);
        ::close(t);
        if (::rename(tmp.c_str(), path_.c_str()) != 0) return fail(errc::internal, "reminder compact: rename failed");
        open_append();
        return {};
    }

private:
    // --- framing: {u32 len, u32 crc32(kind||body), u8 kind, body} ------------------------------
    static std::uint32_t crc32(std::span<const std::byte> data) noexcept {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB8'8320U ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
        }();
        std::uint32_t c = 0xFFFF'FFFFU;
        for (std::byte b : data) c = table[(c ^ static_cast<std::uint8_t>(b)) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFF'FFFFU;
    }
    static void put_u32(std::byte* p, std::uint32_t v) noexcept {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    static std::uint32_t get_u32(const std::byte* p) noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
        return v;
    }
    static void put_i64(std::byte* p, std::int64_t v) noexcept {
        auto u = static_cast<std::uint64_t>(v);
        for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>((u >> (8 * i)) & 0xFF);
    }
    static std::int64_t get_i64(const std::byte* p) noexcept {
        std::uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
        return static_cast<std::int64_t>(u);
    }
    static void append_bytes(std::vector<std::byte>& v, const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        v.insert(v.end(), b, b + n);
    }

    static std::vector<std::byte> encode_key(ReminderKey k) {
        std::vector<std::byte> v;
        std::array<std::byte, 8> t{};
        put_i64(t.data(), static_cast<std::int64_t>(k.actor.type.value)); append_bytes(v, t.data(), 8);
        put_i64(t.data(), static_cast<std::int64_t>(k.actor.key));       append_bytes(v, t.data(), 8);
        put_i64(t.data(), static_cast<std::int64_t>(k.name_hash));       append_bytes(v, t.data(), 8);
        return v;
    }
    static std::vector<std::byte> encode_put(const ReminderRow& r) {
        std::vector<std::byte> v;
        std::array<std::byte, 8> t{};
        put_i64(t.data(), static_cast<std::int64_t>(r.actor.type.value)); append_bytes(v, t.data(), 8);
        put_i64(t.data(), static_cast<std::int64_t>(r.actor.key));        append_bytes(v, t.data(), 8);
        put_i64(t.data(), r.scheduled_due_ns);                            append_bytes(v, t.data(), 8);
        put_i64(t.data(), r.period_ns);                                   append_bytes(v, t.data(), 8);
        std::array<std::byte, 4> n{};
        put_u32(n.data(), static_cast<std::uint32_t>(r.name.size()));     append_bytes(v, n.data(), 4);
        append_bytes(v, r.name.data(), r.name.size());
        put_u32(n.data(), static_cast<std::uint32_t>(r.payload.size()));  append_bytes(v, n.data(), 4);
        append_bytes(v, r.payload.data(), r.payload.size());
        return v;
    }
    static std::optional<ReminderRow> decode_put(std::span<const std::byte> b) {
        if (b.size() < 8 * 4 + 4) return std::nullopt;
        ReminderRow r;
        std::size_t o = 0;
        r.actor.type.value = static_cast<std::uint64_t>(get_i64(b.data() + o)); o += 8;
        r.actor.key        = static_cast<std::uint64_t>(get_i64(b.data() + o)); o += 8;
        r.scheduled_due_ns = get_i64(b.data() + o); o += 8;
        r.period_ns        = get_i64(b.data() + o); o += 8;
        std::uint32_t nlen = get_u32(b.data() + o); o += 4;
        if (o + nlen + 4 > b.size()) return std::nullopt;
        r.name.assign(reinterpret_cast<const char*>(b.data() + o), nlen); o += nlen;
        std::uint32_t plen = get_u32(b.data() + o); o += 4;
        if (o + plen > b.size()) return std::nullopt;
        r.payload.assign(b.data() + o, b.data() + o + plen);
        return r;
    }
    static std::optional<ReminderKey> decode_key(std::span<const std::byte> b) {
        if (b.size() < 24) return std::nullopt;
        ReminderKey k;
        k.actor.type.value = static_cast<std::uint64_t>(get_i64(b.data()));
        k.actor.key        = static_cast<std::uint64_t>(get_i64(b.data() + 8));
        k.name_hash        = static_cast<std::uint64_t>(get_i64(b.data() + 16));
        return k;
    }

    static void sync_data(int fd) noexcept {
#if defined(__linux__)
        ::fdatasync(fd);
#else
        (void)fd;
#endif
    }

    [[nodiscard]] result<void> frame_write(int fd, Kind kind, std::span<const std::byte> body) {
        std::vector<std::byte> framed;
        std::array<std::byte, 4> h{};
        const std::uint32_t len = static_cast<std::uint32_t>(1 + body.size());  // kind byte + body
        // crc covers kind||body
        std::vector<std::byte> kb;
        kb.push_back(static_cast<std::byte>(kind));
        kb.insert(kb.end(), body.begin(), body.end());
        const std::uint32_t crc = crc32(kb);
        put_u32(h.data(), len);  framed.insert(framed.end(), h.begin(), h.end());
        put_u32(h.data(), crc);  framed.insert(framed.end(), h.begin(), h.end());
        framed.insert(framed.end(), kb.begin(), kb.end());
        std::size_t off = 0;
        while (off < framed.size()) {
            ssize_t w = ::write(fd, framed.data() + off, framed.size() - off);
            if (w < 0) return fail(errc::internal, "reminder store: write failed");
            off += static_cast<std::size_t>(w);
        }
        return {};
    }
    [[nodiscard]] result<void> durable_append(Kind kind, std::span<const std::byte> body) {
        if (fd_ < 0) return fail(errc::internal, "reminder store: not open");
        if (auto e = frame_write(fd_, kind, body); !e) return e;
        sync_data(fd_);  // ADR-017 F1p: exactly one fdatasync per durable op
        return {};
    }

    void open_append() {
#if defined(__linux__)
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
    }

    // Replay the log into the live-row map; a torn trailing record (bad len/crc from a crash
    // mid-write) is detected and the file is truncated to the last good record.
    void replay() {
#if defined(__linux__)
        int fd = ::open(path_.c_str(), O_RDONLY, 0644);
        if (fd < 0) return;
        std::vector<std::byte> buf;
        { struct ::stat st{}; if (::fstat(fd, &st) == 0 && st.st_size > 0) {
            buf.resize(static_cast<std::size_t>(st.st_size));
            std::size_t off = 0;
            while (off < buf.size()) {
                ssize_t r = ::read(fd, buf.data() + off, buf.size() - off);
                if (r <= 0) break;
                off += static_cast<std::size_t>(r);
            }
            buf.resize(off);
        } }
        ::close(fd);

        std::size_t pos = 0, good = 0;
        while (pos + 8 <= buf.size()) {
            std::uint32_t len = get_u32(buf.data() + pos);
            std::uint32_t crc = get_u32(buf.data() + pos + 4);
            if (len == 0 || pos + 8 + len > buf.size()) break;  // torn tail
            std::span<const std::byte> kb{buf.data() + pos + 8, len};
            if (crc32(kb) != crc) break;                        // torn / corrupt tail
            auto kind = static_cast<Kind>(static_cast<std::uint8_t>(kb[0]));
            std::span<const std::byte> body{kb.data() + 1, kb.size() - 1};
            if (kind == Kind::kPut) { if (auto r = decode_put(body)) rows_[r->key()] = std::move(*r); }
            else if (kind == Kind::kRemove) { if (auto k = decode_key(body)) rows_.erase(*k); }
            else if (kind == Kind::kCheckpoint) { if (body.size() >= 8) checkpoint_ = get_i64(body.data()); }
            pos += 8 + len;
            good = pos;
        }
        if (good < buf.size()) {  // torn tail — truncate so future appends start clean
            int t = ::open(path_.c_str(), O_WRONLY, 0644);
            if (t >= 0) { if (::ftruncate(t, static_cast<off_t>(good)) != 0) { /* best-effort */ } ::close(t); }
        }
#endif
    }

    std::string path_;
    int fd_ = -1;
    std::unordered_map<ReminderKey, ReminderRow> rows_;
    std::int64_t checkpoint_ = -1;
};
static_assert(ReminderStore<FileReminderStore>, "FileReminderStore must model ReminderStore");

// ============================================================================================
// SEGSTREAM ReminderService (ADR-017 winner). Ordered due-index (O(due-now) scan) + token-bucket
// drain (peak fire == fire_rate, exact) + completion-gated durable confirmation (zero loss) +
// deterministic drain-rank smear (restart-stable).
// ============================================================================================
struct ReminderConfig {
    std::int64_t bucket_ns = 1'000'000'000;  // B — coarse due-index granularity (1 s; ADR-017)
    // The FIRE RATE (reminders/sec) — the drain rate that IS the spread lever. A mass-due wave of N
    // drains at this rate, so the peak dispatch is `fire_rate` EXACTLY, not N (ADR-017 C4p). 0 ⇒ no
    // cap: fire everything due immediately (the fast path for low-cardinality reminders, ADR-017 F4).
    std::uint32_t fire_rate = 0;
    // kCredit — max reminders dispatched per tick (bounds per-tick work / models the in-flight
    // credit window; ADR-017 S1p peak in-flight == kCredit). Also caps a fire_rate==0 fast-path tick.
    std::uint32_t credit = 4096;
};

template <ReminderStore StoreT>
class ReminderService {
public:
    using FireFn = std::function<void(const FireEvent&)>;

    ReminderService(StoreT& store, FireFn on_fire, ReminderConfig cfg = {})
        : store_(store), on_fire_(std::move(on_fire)), cfg_(cfg) {}

    // COLD OPEN — rebuild the in-RAM due-index from the durable rows (ADR-017: O(total-owned); a
    // compacted store bounds this to O(live)). Rows already resolved were physically removed/advanced,
    // so what loads is exactly the still-pending set; overdue rows fire as catch-up on the next tick.
    void open() {
        index_.clear();
        auto rows = store_.load_all();
        if (!rows) return;
        for (auto& r : *rows) insert_index(r);
        replayed_ = rows->size();
    }

    // Register / re-register (UPSERT) a reminder. One durable write (ADR-017 F1p). Named, so a handler
    // that re-arms every run does not accumulate duplicates. Returns the fenced/failed store error.
    [[nodiscard]] result<void> remind_at(ActorId actor, std::string name, WallInstant due,
                                         std::int64_t period_ns, std::vector<std::byte> payload) {
        ReminderRow row{actor, std::move(name), due.ns, period_ns, std::move(payload)};
        ReminderKey k = row.key();
        if (auto e = store_.put(row); !e) return e;   // durable first
        // Drop any stale in-RAM index entry for this key (upsert semantics) by bumping its epoch.
        by_key_[k] = row;
        insert_index(row);
        return {};
    }
    [[nodiscard]] result<void> remind_after(ActorId actor, std::string name, WallInstant now_wall,
                                            std::int64_t after_ns, std::int64_t period_ns,
                                            std::vector<std::byte> payload) {
        return remind_at(actor, std::move(name), WallInstant{now_wall.ns + after_ns}, period_ns,
                         std::move(payload));
    }
    [[nodiscard]] result<void> cancel(ActorId actor, std::string_view name) {
        ReminderKey k{actor, reminder_name_hash(name)};
        by_key_.erase(k);                              // in-RAM: lazily skipped in the index on drain
        return store_.remove(k);                       // durable
    }

    // Advance the service to wall instant `now`, draining the due segment under the token bucket.
    // O(due-now): only buckets ≤ now are pulled from the ordered index (begin-peek), never the whole
    // population. Returns the number fired this tick.
    std::size_t tick(WallInstant now) {
        // (1) Pull all buckets that have come due into the ready segment, then order the newly-added
        //     rows by drain_rank so the smear is deterministic and restart-stable (ADR-017 C4p).
        const std::int64_t now_bucket = bucket_of(now.ns);
        std::size_t added = 0;
        for (auto it = index_.begin(); it != index_.end() && it->first <= now_bucket;) {
            for (const ReminderKey& key : it->second) { ready_.push_back(ReadyItem{key, drain_rank(key)}); ++added; }
            it = index_.erase(it);
        }
        if (added > 0)
            std::sort(ready_.begin(), ready_.end(), [](const ReadyItem& a, const ReadyItem& b) {
                return a.rank < b.rank;
            });

        // (2) Grant tokens for the elapsed wall time and drain up to that many (≤ kCredit) this tick.
        //     fire_rate == 0 ⇒ unlimited (fast path, capped only by kCredit per tick).
        std::size_t budget;
        if (cfg_.fire_rate == 0) {
            budget = ready_.size() < cfg_.credit ? ready_.size() : cfg_.credit;
        } else {
            // Accrue tokens at fire_rate per second. The FIRST tick has no prior instant to measure
            // from, so it is granted exactly one bucket's worth — ticking at the natural per-bucket
            // cadence then grants `fire_rate * bucket_seconds` every tick, i.e. peak == fire_rate
            // per second EXACTLY from the first tick (ADR-017 C4p), with no dead opening tick.
            const double bucket_seconds = static_cast<double>(cfg_.bucket_ns) / 1e9;
            const double dt = last_tick_ns_ < 0 ? bucket_seconds
                                                : static_cast<double>(now.ns - last_tick_ns_) / 1e9;
            tokens_ += dt * static_cast<double>(cfg_.fire_rate);
            const double cap = static_cast<double>(cfg_.credit);
            if (tokens_ > cap) tokens_ = cap;           // burst ceiling = credit window
            std::size_t grant = tokens_ > 0 ? static_cast<std::size_t>(tokens_) : 0;
            budget = std::min<std::size_t>(grant, cfg_.credit);
        }
        last_tick_ns_ = now.ns;

        std::size_t fired = 0, drained = 0;
        for (; drained < ready_.size() && fired < budget; ++drained) {
            const ReadyItem& item = ready_[drained];
            auto rit = by_key_.find(item.key);
            if (rit == by_key_.end()) continue;         // canceled — lazily skipped
            const ReminderRow& row = rit->second;

            // (3) FIRE — model the `tell` on the actor's own lane (011 local delivery vehicle).
            FireEvent ev{row.actor, row.name, item.key.name_hash, row.scheduled_due_ns, now.ns,
                         {row.payload.data(), row.payload.size()}};
            on_fire_(ev);
            ++fired; ++fired_total_;

            // (4) COMPLETION-GATED durable confirmation — AFTER the fire (ADR-017 S2p). A crash before
            //     this leaves the old row ⇒ re-fires with the same (name, scheduled_due) on reopen.
            if (row.period_ns > 0) {
                ReminderRow next = row;
                next.scheduled_due_ns = row.scheduled_due_ns + row.period_ns;  // from SCHEDULED, no drift
                (void)store_.put(next);                 // advance durably
                by_key_[item.key] = next;
                insert_index(next);
            } else {
                (void)store_.remove(item.key);          // one-shot — resolved
                by_key_.erase(item.key);
            }
            if (cfg_.fire_rate != 0) tokens_ -= 1.0;
        }
        // Drop the drained prefix.
        if (drained > 0) ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(drained));

        // (5) Checkpoint once the wave for buckets ≤ now is fully drained (rebuild-skip optimization).
        if (ready_.empty() && index_.empty()) (void)store_.checkpoint(now_bucket);
        return fired;
    }

    // --- introspection (tests / benches; not the hot path) ------------------------------------
    [[nodiscard]] std::size_t pending() const noexcept { return by_key_.size(); }
    [[nodiscard]] std::size_t ready_size() const noexcept { return ready_.size(); }
    [[nodiscard]] std::uint64_t fired_total() const noexcept { return fired_total_; }
    [[nodiscard]] std::size_t replayed() const noexcept { return replayed_; }
    [[nodiscard]] std::int64_t bucket_of(std::int64_t ns) const noexcept { return ns / cfg_.bucket_ns; }
    [[nodiscard]] static std::uint64_t drain_rank(ReminderKey k) noexcept {
        return splitmix64(k.actor.hash() ^ (k.name_hash * 0x9E37'79B9'7F4A'7C15ULL));
    }

private:
    struct ReadyItem { ReminderKey key; std::uint64_t rank; };

    void insert_index(const ReminderRow& row) {
        by_key_[row.key()] = row;  // keep the live copy for fire + upsert
        index_[bucket_of(row.scheduled_due_ns)].push_back(row.key());
    }

    StoreT& store_;
    FireFn on_fire_;
    ReminderConfig cfg_;
    // Ordered due-index: bucket → keys due in it. std::map ⇒ begin-peek is O(log S), so a tick that
    // pulls only due buckets is O(due-now), independent of total population (ADR-017 F2).
    std::map<std::int64_t, std::vector<ReminderKey>> index_;
    std::unordered_map<ReminderKey, ReminderRow> by_key_;  // live rows (upsert / cancel / fire lookup)
    std::vector<ReadyItem> ready_;                         // the due segment, drained under the token bucket
    double tokens_ = 0.0;
    std::int64_t last_tick_ns_ = -1;
    std::uint64_t fired_total_ = 0;
    std::size_t replayed_ = 0;
};

}  // namespace quark
