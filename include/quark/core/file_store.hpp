// Implements 012-Persistence — the first CRASH-DURABLE `Store` adapter: `FileStore`. It is the
// std-only default the README dependency table names ("append-only WAL + snapshots on local FS"),
// and it plugs into the exact same `Store` seam as the reference `InMemoryStore` — the snapshot /
// event-log / recovery / fencing MECHANISM (snapshot.hpp, event_log.hpp) is untouched; only the
// backing store changes.
//
// DURABILITY MODEL. One append-only write-ahead log FILE per actor, `<root>/<type>-<key>.qwal`.
// Every durable mutation (fence acquire, snapshot, event batch) appends ONE self-describing,
// CRC32-framed record and `fdatasync`s before returning — so an acknowledged write survives process
// exit / power loss (the 012 §Sync durability contract). On open the file is replayed to rebuild the
// in-RAM index (owner token, last_seq, latest snapshot, event log); a TORN TRAILING RECORD from a
// crash mid-write (bad length or CRC) is detected and truncated away, so recovery never observes a
// partial write. Reads are served from the in-RAM index (fast); the disk is the durability journal.
//
// ATOMICITY. `append_batch` is written as a SINGLE framed record (count + all events) with one CRC
// and one `fdatasync`, so a crash leaves either the whole batch durable or none of it — the
// all-or-none contract 012/ADR-009 C7 requires, enforced at the disk layer, not just in RAM.
//
// FENCING. The current owner token is journaled (a FENCE record) and reconstructed on open, so a
// superseded (stale-token) writer stays fenced out ACROSS a restart, not just within one process —
// the split-brain guarantee is durable.
//
// SCOPE / PORTABILITY. Backed by the 019 PAL durable-file seam (pal/file_io.hpp: `file_open`/
// `file_pwrite`/`file_pread`/`file_truncate`/`durable_flush`) — POSIX (`pwrite`/`fdatasync`/
// `ftruncate`) on Linux, Win32 (`WriteFile`/`FlushFileBuffers`/`SetEndOfFile`) on Windows, both
// behind the identical `pal::file_*` surface (019 §"The one rule"). The durable byte format is
// host-endian — correct on the x86-64 target; a portable version byte-swaps the fixed-width
// integers, a noted seam. Compaction (truncating the log prefix a snapshot subsumes) is a documented
// future optimization — correctness never depends on it (recovery reads snapshot + tail-from-
// through_seq), only file size does.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pal/file_io.hpp"
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"

namespace quark {

// ============================================================================================
// FileStore — the crash-durable local-filesystem `Store` adapter (012). Models the `Store` concept.
// ============================================================================================
class FileStore {
public:
    // Open (creating if absent) a store rooted at `dir`. Existing per-actor logs are replayed lazily
    // on first touch of each ActorId (not eagerly here), so opening a store with a million actors is
    // O(1); each actor pays its own one-time replay on first use.
    explicit FileStore(std::string dir) : root_(std::move(dir)) {
        pal::make_dir(root_);  // idempotent — a missing/unwritable dir surfaces at open() instead
    }
    FileStore(const FileStore&) = delete;
    FileStore& operator=(const FileStore&) = delete;

    // 012 fencing: journal + hand out a strictly-greater token, recorded as the current owner of `id`.
    //
    // THREAD SAFETY (012 adapters serialize access, review B3): every public method takes `mu_`
    // (it guards `table_`, each Entry's in-RAM index and its write offset).
    [[nodiscard]] result<FenceToken> acquire_fence(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        Entry& e = entry(id);
        if (e.corrupt) return corrupt_error();  // unusable log: no token
        const FenceToken next{e.owner.value + 1};
        std::vector<std::byte> body;
        put_u8(body, kFence);
        put_u64(body, next.value);
        // Durability failed: FAIL, do not hand out a token. Returning the current owner's token let
        // two activations share one token, defeating the fence (review B5).
        if (!durable_append(e, body))
            return fail(errc::internal, "FileStore: could not durably record the new fence owner");
        e.owner = next;
        return e.owner;
    }

    [[nodiscard]] SeqNo last_seq(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return entry(id).last_seq;
    }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        Entry& e = entry(id);
        if (e.corrupt) return corrupt_error();
        if (!e.snapshot.has_value()) return std::optional<SnapshotRecord>{};
        return std::optional<SnapshotRecord>{e.snapshot};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        std::lock_guard<std::mutex> g(mu_);
        Entry& e = entry(id);
        if (e.corrupt) return corrupt_error();
        if (is_unfenced(tok) || tok < e.owner) return fenced_error();  // no/superseded token
        if (e.snapshot.has_value() && snap.through_seq < e.snapshot->through_seq)  // review B11
            return fail(errc::internal, "stale snapshot: through_seq older than the stored snapshot");
        if (snap.through_seq > e.last_seq)                           // 012: never ahead of the tail
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
        std::vector<std::byte> body;
        put_u8(body, kSnapshot);
        put_u64(body, tok.value);
        put_u64(body, snap.through_seq);
        put_blob(body, snap.record);
        if (auto rc = durable_append_r(e, body); !rc) return rc;
        SnapshotRecord copy = snap;
        copy.fence = tok;
        e.snapshot = std::move(copy);
        return {};
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        EventRecord one{seq, std::vector<std::byte>(bytes.begin(), bytes.end())};
        return append_batch(id, tok, std::span<const EventRecord>(&one, 1));
    }

    // ATOMIC batch append (012/ADR-009 C7): validate the WHOLE batch (fence + strict seq monotonicity)
    // before writing, then write ONE framed record + one fdatasync ⇒ all-or-none on disk.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        std::lock_guard<std::mutex> g(mu_);
        Entry& e = entry(id);
        if (e.corrupt) return corrupt_error();
        if (is_unfenced(tok) || tok < e.owner) return fenced_error();  // reject the whole batch
        SeqNo prev = e.last_seq;
        for (const EventRecord& r : batch) {
            if (r.seq <= prev) return fail(errc::internal, "commit sequence not strictly increasing");
            prev = r.seq;
        }
        std::vector<std::byte> body;
        put_u8(body, kBatch);
        put_u32(body, static_cast<std::uint32_t>(batch.size()));
        for (const EventRecord& r : batch) {
            put_u64(body, r.seq);
            put_blob(body, r.record);
        }
        if (auto rc = durable_append_r(e, body); !rc) return rc;     // no in-RAM mutation on failure
        for (const EventRecord& r : batch) e.log.push_back(r);
        e.last_seq = prev;
        return {};
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) {
        std::lock_guard<std::mutex> g(mu_);
        Entry& e = entry(id);
        if (e.corrupt) return corrupt_error();
        EventCursor cur;
        for (const auto& ev : e.log)
            if (ev.seq >= from) cur.entries.push_back(ev);
        return cur;
    }

    // --- Test/diagnostic introspection (not part of the Store seam) ---------------------------
    [[nodiscard]] FenceToken current_owner(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return entry(id).owner;
    }
    [[nodiscard]] std::size_t log_size(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return entry(id).log.size();
    }
    // True if `id`'s log failed its integrity check mid-file (a CRC-bad record with intact data after
    // it). Such a log is NEVER truncated or served: every operation on it errors (review B7).
    [[nodiscard]] bool corrupted(ActorId id) {
        std::lock_guard<std::mutex> g(mu_);
        return entry(id).corrupt;
    }

private:
    // Record kinds in the WAL payload (after the {len, crc} frame header).
    static constexpr std::uint8_t kFence = 1;
    static constexpr std::uint8_t kSnapshot = 2;
    static constexpr std::uint8_t kBatch = 3;

    struct Entry {
        pal::file_t fd = pal::invalid_file;
        std::uint64_t write_off = 0;         // append offset (also the truncation point after replay)
        FenceToken owner{};
        SeqNo last_seq = 0;
        std::optional<SnapshotRecord> snapshot;
        std::vector<EventRecord> log;
        bool corrupt = false;  // mid-file corruption detected on replay: refuse to serve or truncate
        ~Entry() { pal::file_close(fd); }
    };

    [[nodiscard]] static std::unexpected<error> corrupt_error() noexcept {
        return fail(errc::internal,
                    "FileStore: log is corrupt mid-file (CRC mismatch before intact records); "
                    "refusing to serve or truncate it; restore from backup / repair offline");
    }

    // Lazy open + replay of `id`'s WAL into an in-RAM Entry (one-time per actor).
    Entry& entry(ActorId id) {
        auto it = table_.find(id);
        if (it != table_.end()) return *it->second;
        auto e = std::make_unique<Entry>();
        const std::string path = path_for(id);
        e->fd = pal::file_open(path, pal::FileOpenMode::kReadWriteCreate);
        if (e->fd != pal::invalid_file) {
            replay(*e);
            // A brand-new (empty) log: fdatasync on the file never makes its DIRECTORY ENTRY durable, so
            // a crash could lose the whole file though every record in it was acknowledged (review B9).
            if (e->write_off == 0 && !e->corrupt) (void)pal::durable_sync_dir(root_);
        }
        Entry& ref = *e;
        table_.emplace(id, std::move(e));
        return ref;
    }

    // Read the whole file and rebuild {owner, last_seq, snapshot, log}, stopping at the first torn
    // record (short read or CRC mismatch) and truncating the file to the last intact record.
    void replay(Entry& e) {
        std::vector<std::byte> file = read_all(e.fd);
        std::uint64_t off = 0;
        const std::uint64_t n = file.size();
        for (;;) {
            if (off + 8 > n) break;  // no room for a frame header ⇒ end (or torn)
            std::uint32_t len = 0, crc = 0;
            std::memcpy(&len, file.data() + off, 4);
            std::memcpy(&crc, file.data() + off + 4, 4);
            const std::uint64_t body_off = off + 8;
            if (body_off + len > n) break;                          // torn: body not fully written
            std::span<const std::byte> body(file.data() + body_off, len);
            if (crc32(body) != crc) {
                // A torn append only damages the TAIL: a bad record that ends exactly at EOF is that.
                // A bad record with more data after it is MID-FILE corruption (bit-rot / bad sector):
                // truncating here would durably delete every later acknowledged record, so keep the
                // file untouched and mark the log unusable instead (review B7).
                if (body_off + len < n) e.corrupt = true;
                break;
            }
            if (!apply_record(e, body)) {
                // CRC-valid but malformed / unknown / non-monotonic content (review B8): a CRC is not a
                // MAC and a torn write cannot produce a valid CRC, so this is corruption, not a tail.
                e.corrupt = true;
                break;
            }
            off = body_off + len;                                   // advance past this good record
        }
        if (e.corrupt) {
            e.write_off = n;  // never append behind (or truncate) a corrupt region
            return;
        }
        if (off != n) (void)pal::file_truncate(e.fd, off);  // drop a torn tail
        e.write_off = off;
    }

    // Apply one replayed record to the in-RAM Entry (records are in append order). Every read is
    // bounds-checked against the record (review B8): returns false — leaving the caller to mark the log
    // corrupt — for a truncated field, an oversized blob/count, an unknown kind, trailing bytes, or a
    // batch whose seqs are not strictly increasing (the same invariant append_batch enforces).
    [[nodiscard]] static bool apply_record(Entry& e, std::span<const std::byte> body) {
        Reader r{body};
        const std::uint8_t kind = r.u8();
        if (kind == kFence) {
            const std::uint64_t tok = r.u64();
            if (!r.ok || !r.done()) return false;
            e.owner = FenceToken{tok};
        } else if (kind == kSnapshot) {
            SnapshotRecord s;
            s.fence = FenceToken{r.u64()};
            s.through_seq = r.u64();
            s.record = r.blob();
            if (!r.ok || !r.done()) return false;
            e.snapshot = std::move(s);       // latest snapshot wins (later record overwrites)
        } else if (kind == kBatch) {
            const std::uint32_t count = r.u32();
            SeqNo prev = e.last_seq;
            std::vector<EventRecord> evs;    // staged: a bad record must not half-apply
            for (std::uint32_t i = 0; i < count && r.ok; ++i) {
                EventRecord ev;
                ev.seq = r.u64();
                ev.record = r.blob();
                if (!r.ok) break;
                if (ev.seq <= prev) return false;   // non-monotonic replay
                prev = ev.seq;
                evs.push_back(std::move(ev));
            }
            if (!r.ok || !r.done()) return false;
            for (auto& ev : evs) e.log.push_back(std::move(ev));
            e.last_seq = prev;
        } else {
            return false;  // unknown kind: refuse rather than guess (forward-compat skipping is a seam)
        }
        return true;
    }

    // Frame `body` as {u32 len, u32 crc32(body), body}, write it, and fdatasync. All-or-none per call.
    [[nodiscard]] bool durable_append(Entry& e, const std::vector<std::byte>& body) {
        std::vector<std::byte> frame;
        frame.reserve(body.size() + 8);
        put_u32(frame, static_cast<std::uint32_t>(body.size()));
        put_u32(frame, crc32(std::span<const std::byte>(body)));
        frame.insert(frame.end(), body.begin(), body.end());
        const std::int64_t w = pal::file_pwrite(e.fd, frame.data(), frame.size(), e.write_off);
        if (w != static_cast<std::int64_t>(frame.size())) return false;  // short/failed write
        if (!pal::durable_flush(e.fd)) return false;                     // durability barrier failed
        e.write_off += frame.size();
        return true;
    }
    // result<void> flavor for the callers that surface an error value.
    [[nodiscard]] result<void> durable_append_r(Entry& e, const std::vector<std::byte>& body) {
        if (!durable_append(e, body)) return fail(errc::internal, "FileStore: durable write failed");
        return {};
    }

    [[nodiscard]] std::string path_for(ActorId id) const {
        char name[64];
        std::snprintf(name, sizeof(name), "/%016llx-%016llx.qwal",
                      static_cast<unsigned long long>(id.type.value),
                      static_cast<unsigned long long>(id.key));
        return root_ + name;
    }

    static std::vector<std::byte> read_all(pal::file_t fd) {
        std::vector<std::byte> out;
        std::byte buf[65536];
        std::uint64_t off = 0;
        for (;;) {
            const std::int64_t r = pal::file_pread(fd, buf, sizeof(buf), off);
            if (r <= 0) break;
            out.insert(out.end(), buf, buf + r);
            off += static_cast<std::uint64_t>(r);
        }
        return out;
    }

    // ---- fixed-width host-endian (LE on x86-64) put/get helpers over a byte buffer ----------
    static void put_u8(std::vector<std::byte>& b, std::uint8_t v) { b.push_back(std::byte{v}); }
    static void put_u32(std::vector<std::byte>& b, std::uint32_t v) { put_raw(b, &v, 4); }
    static void put_u64(std::vector<std::byte>& b, std::uint64_t v) { put_raw(b, &v, 8); }
    static void put_blob(std::vector<std::byte>& b, const std::vector<std::byte>& v) {
        put_u32(b, static_cast<std::uint32_t>(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    }
    static void put_raw(std::vector<std::byte>& b, const void* p, std::size_t n) {
        const auto* s = static_cast<const std::byte*>(p);
        b.insert(b.end(), s, s + n);
    }
    // Bounds-checked cursor over one record body: any over-read clears `ok` and yields zeros/empty,
    // never touching bytes outside the span (review B8).
    struct Reader {
        std::span<const std::byte> b;
        std::uint64_t p = 0;
        bool ok = true;
        [[nodiscard]] bool done() const noexcept { return p == b.size(); }
        [[nodiscard]] bool need(std::uint64_t n) noexcept {
            if (!ok || n > b.size() - p) { ok = false; return false; }
            return true;
        }
        std::uint8_t u8() {
            if (!need(1)) return 0;
            return static_cast<std::uint8_t>(b[p++]);
        }
        std::uint32_t u32() {
            std::uint32_t v = 0;
            if (!need(4)) return 0;
            std::memcpy(&v, b.data() + p, 4); p += 4; return v;
        }
        std::uint64_t u64() {
            std::uint64_t v = 0;
            if (!need(8)) return 0;
            std::memcpy(&v, b.data() + p, 8); p += 8; return v;
        }
        std::vector<std::byte> blob() {
            const std::uint32_t len = u32();
            if (!need(len)) return {};
            std::vector<std::byte> v(b.begin() + static_cast<std::ptrdiff_t>(p),
                                     b.begin() + static_cast<std::ptrdiff_t>(p) + len);
            p += len;
            return v;
        }
    };

    // CRC32 (IEEE 802.3) over the record body — torn/corrupt trailing-record detection on replay.
    static std::uint32_t crc32(std::span<const std::byte> data) noexcept {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
        }();
        std::uint32_t c = 0xFFFF'FFFFu;
        for (std::byte b : data)
            c = table[(c ^ static_cast<std::uint8_t>(b)) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFF'FFFFu;
    }

    std::string root_;
    std::mutex mu_;
    std::unordered_map<ActorId, std::unique_ptr<Entry>> table_;
};

static_assert(Store<FileStore>, "FileStore must model the 012 Store seam");

}  // namespace quark
