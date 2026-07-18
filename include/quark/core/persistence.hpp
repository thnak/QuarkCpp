// Implements 012-Persistence — the opt-in durability seam: the `Store` adapter concept, the
// `PersistMode`/`Persistent<Model, Mode>` policy surface, the monotonic commit sequence number,
// the fencing token that stops a split-brain double-writer, and the reference `Store` adapter
// (`InMemoryStore`). Bound to ADR-009 (EventSourced staging fence + Restart reload) and 016
// (durable records are the canonical tagged encoding, prefixed {type_key, schema_version},
// migrated on read).
//
// SCOPE. This header defines ONLY the storage seam + policy surface + the reference adapter. The
// serialize/snapshot mechanism is in `snapshot.hpp`; the event-sourced staging/commit/replay
// mechanism is in `event_log.hpp`. Nothing here is on the hot path — persistence work happens at
// activation (recovery), at handler-completion commit points, and at snapshot checkpoints, all off
// the sequential drain path. The `Store` seam is a COLD adapter boundary, so a runtime-typed
// `StateStore` base (spec 012 §"Storage seam") is expressible over it, but the primary seam is the
// zero-erasure `Store` concept so the default in-process store pays no virtual dispatch.
//
// `InMemoryStore` (below) is the REFERENCE store: a LOGICAL fence gate + an in-RAM append-only log +
// latest snapshot. It exercises every seam property a real backend must honour (fencing, strict seq
// monotonicity, atomic batch append) but is NOT durable across process exit. The crash-durable
// adapters live OUTSIDE this header, behind the same `Store` seam, so the log/commit/replay mechanism
// is untouched: `FileStore` (append-only WAL + `fdatasync`, std-only, shipped + verified —
// `quark/core/file_store.hpp`), and the opt-in library backends `SqliteStore` / `RocksStore`
// (`quark/adapters/…`, built when `QUARK_WITH_SQLITE`/`QUARK_WITH_ROCKSDB`). See PersistenceAdapters.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"

namespace quark {

// ============================================================================================
// 012 §"When state is persisted" — the per-actor durability mode (a compile-time policy knob).
// ============================================================================================
enum class PersistMode : std::uint8_t {
    Sync,     // persist the mutation before the message completes / before an `ask` reply is sent
    Batched,  // persist asynchronously, coalescing writes; ack before durable (bounded loss window)
};

// 012 §"Two models" — the durability model, chosen per actor. Snapshot stores the latest serialized
// state; EventSourced stores an append-only log of state-changing events (+ periodic snapshot as a
// compaction checkpoint). These are declared-surface TAG types.
struct Snapshot {};      // latest-state model
struct EventSourced {};  // append-only event log model

// The persistence policy tag (spec 012 example: `Persistent<Snapshot, PersistMode::Sync>`). This is
// the DECLARED SURFACE — detecting it inside the `Actor<>` CRTP policy pack (005 policies.hpp) and
// driving recovery from activation (008/metadata) is a reported seam that lives in those core
// headers; this header owns the tag + its trait extractors so the mechanism is fully usable and
// testable standalone.
template <class Model, PersistMode Mode = PersistMode::Sync>
struct Persistent {
    using model = Model;
    static constexpr PersistMode mode = Mode;
    static_assert(std::is_same_v<Model, Snapshot> || std::is_same_v<Model, EventSourced>,
                  "Persistent<Model,…>: Model must be quark::Snapshot or quark::EventSourced");
};

// --- Trait extractors over the policy tag ----------------------------------------------------
template <class T>
struct is_persistent_policy : std::false_type {};
template <class Model, PersistMode Mode>
struct is_persistent_policy<Persistent<Model, Mode>> : std::true_type {};
template <class T>
inline constexpr bool is_persistent_policy_v = is_persistent_policy<T>::value;

template <class T>
struct is_event_sourced_policy : std::false_type {};
template <PersistMode Mode>
struct is_event_sourced_policy<Persistent<EventSourced, Mode>> : std::true_type {};
template <class T>
inline constexpr bool is_event_sourced_v = is_event_sourced_policy<T>::value;

// ============================================================================================
// 012 §"Placement, mobility, and fencing" + ADR-009 — the sequence number and the fencing token.
// ============================================================================================

// The monotonically increasing COMMIT sequence number (012 / ADR-009 C7). Each committed event —
// and each snapshot checkpoint — carries one; strictly increasing per actor across restarts, so
// replay is totally ordered and a snapshot names the exact log prefix it subsumes.
using SeqNo = std::uint64_t;

// The FENCING token (012 §"Placement, mobility, and fencing"). Each activation acquires a
// monotonically increasing token, persisted with the state; the store REJECTS writes carrying a
// stale token, so a partitioned/superseded old activation cannot corrupt state after a newer one
// takes over. The bump happens on reconstruct (ADR-009 Restart-reload rule).
struct FenceToken {
    std::uint64_t value = 0;
    friend constexpr bool operator==(FenceToken, FenceToken) = default;
    friend constexpr bool operator<(FenceToken a, FenceToken b) noexcept { return a.value < b.value; }
};

// A durable snapshot record as the store sees it: the fence token that wrote it, the commit
// sequence it subsumes (`through_seq` — every event with seq ≤ this is folded into `record`), and
// the 016 canonical-tagged bytes of the serialized state (header {type_key, schema_version} + body).
struct SnapshotRecord {
    FenceToken fence{};
    SeqNo through_seq = 0;
    std::vector<std::byte> record;  // encode_record(state) — durable canonical tagged bytes (016)
};

// One durable event as the store sees it: its commit sequence and the 016 canonical-tagged bytes.
struct EventRecord {
    SeqNo seq = 0;
    std::vector<std::byte> record;  // encode_record(event) — durable canonical tagged bytes (016)
};

// The read cursor over the tail log (012 §"Storage seam" `read_log`). A value type carrying the
// events with `seq >= from` in strictly-increasing seq order — the reference adapters materialize
// it eagerly; a DB adapter may stream, but the replay loop only needs forward iteration.
struct EventCursor {
    std::vector<EventRecord> entries;
    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] auto begin() const noexcept { return entries.begin(); }
    [[nodiscard]] auto end() const noexcept { return entries.end(); }
};

// ============================================================================================
// 012 §"Storage seam" — the `Store` concept. The one seam behind which the default WAL and any
// heavier backend (RocksDB/SQL/object storage) plug in. The concept, not a virtual base, is the
// primary form so the default in-process store pays no dispatch; a runtime `StateStore` erasure is
// expressible over it for adapters that need heterogeneous stores in one array.
// ============================================================================================
//
// FENCING CONTRACT. `acquire_fence(id)` returns a token strictly greater than every token yet
// issued for `id`, and marks it the current owner. `save_snapshot`/`append` carry a token; the
// store rejects (errc::unavailable) any write whose token is OLDER than the current owner — that is
// the split-brain fence. A write from the current owner is accepted. `last_seq(id)` reports the
// highest committed sequence so a recovering activation continues the count.
template <class S>
concept Store = requires(S& s, ActorId id, FenceToken tok, SeqNo seq,
                         std::span<const std::byte> bytes, std::span<const EventRecord> batch,
                         const SnapshotRecord& snap) {
    { s.acquire_fence(id) } -> std::same_as<FenceToken>;
    { s.last_seq(id) } -> std::same_as<SeqNo>;
    { s.load_snapshot(id) } -> std::same_as<result<std::optional<SnapshotRecord>>>;
    { s.save_snapshot(id, tok, snap) } -> std::same_as<result<void>>;
    { s.append(id, tok, seq, bytes) } -> std::same_as<result<void>>;
    // ATOMIC batch append (012/ADR-009 C7). Appends EVERY record or NONE — the store must validate
    // the whole batch (fence + strict seq monotonicity) BEFORE mutating any durable state, so a
    // mid-batch failure leaves the log unchanged (no torn partial commit). `EventLog::commit()`
    // drives this once per handler-completion so a multi-event commit is all-or-nothing.
    { s.append_batch(id, tok, batch) } -> std::same_as<result<void>>;
    { s.read_log(id, seq) } -> std::same_as<result<EventCursor>>;
};

// The fence-rejection error (012): a stale/superseded writer's write is refused. `unavailable` is
// the taxonomy slot for "a newer owner fenced this activation out" (007/010 partition semantics).
[[nodiscard]] inline std::unexpected<error> fenced_error() noexcept {
    return fail(errc::unavailable, "fenced: write from a superseded (stale-token) writer");
}

// ============================================================================================
// The reference adapter — `InMemoryStore`. Log semantics (append-only log + latest snapshot + LOGICAL
// fence gate + strict seq monotonicity + atomic batch append) with the disk swapped for RAM, so it
// exercises every seam property a real backend must honour. NOT durable across process exit — the
// crash-durable adapters (`FileStore`, `SqliteStore`, `RocksStore`) implement this same `Store` seam
// elsewhere (see the file header / PersistenceAdapters.md).
// ============================================================================================
class InMemoryStore {
public:
    // 012 fencing: hand out a strictly-greater token and record it as the current owner of `id`.
    [[nodiscard]] FenceToken acquire_fence(ActorId id) {
        auto& e = table_[id];
        e.owner = FenceToken{e.owner.value + 1};
        return e.owner;
    }

    [[nodiscard]] SeqNo last_seq(ActorId id) const noexcept {
        const auto it = table_.find(id);
        return it == table_.end() ? SeqNo{0} : it->second.last_seq;
    }

    [[nodiscard]] result<std::optional<SnapshotRecord>> load_snapshot(ActorId id) const {
        const auto it = table_.find(id);
        if (it == table_.end() || !it->second.snapshot.has_value()) return std::optional<SnapshotRecord>{};
        return std::optional<SnapshotRecord>{it->second.snapshot};
    }

    [[nodiscard]] result<void> save_snapshot(ActorId id, FenceToken tok, const SnapshotRecord& snap) {
        auto& e = table_[id];
        if (tok < e.owner) return fenced_error();       // superseded writer — reject
        // 012: a snapshot names the EXACT log prefix it subsumes, so `through_seq` must never run
        // ahead of the actual appended tail. An over-advanced through_seq would make recovery skip
        // un-replayed seqs (silent data loss) AND jump `last_seq` past never-appended seqs, wedging
        // future appends. Reject it (leave the store unchanged) rather than trust the caller.
        if (snap.through_seq > e.last_seq)
            return fail(errc::internal, "snapshot through_seq ahead of the appended tail");
        SnapshotRecord copy = snap;
        copy.fence = tok;
        e.snapshot = std::move(copy);
        return {};
    }

    [[nodiscard]] result<void> append(ActorId id, FenceToken tok, SeqNo seq,
                                      std::span<const std::byte> bytes) {
        auto& e = table_[id];
        if (tok < e.owner) return fenced_error();       // superseded writer — reject
        // Commit sequence numbers are strictly increasing (012/ADR-009 C7); event seqs start at 1,
        // so a seq that does not advance past the last committed one is a caller bug, never normal.
        if (seq <= e.last_seq) return fail(errc::internal, "commit sequence not strictly increasing");
        e.log.push_back(EventRecord{seq, std::vector<std::byte>(bytes.begin(), bytes.end())});
        e.last_seq = seq;
        return {};
    }

    // ATOMIC batch append (012/ADR-009 C7) — appends EVERY record or NONE. VALIDATE the whole batch
    // first (fence, then strict seq monotonicity over the last committed seq AND over each
    // predecessor); only if the entire batch is valid do we push, so a rejected batch mutates NO
    // durable state (no torn partial commit). `EventLog::commit()` calls this once per handler
    // completion, making a multi-event commit all-or-nothing.
    [[nodiscard]] result<void> append_batch(ActorId id, FenceToken tok,
                                            std::span<const EventRecord> batch) {
        auto& e = table_[id];
        if (tok < e.owner) return fenced_error();       // superseded writer — reject the whole batch
        SeqNo prev = e.last_seq;
        for (const EventRecord& r : batch) {
            if (r.seq <= prev) return fail(errc::internal, "commit sequence not strictly increasing");
            prev = r.seq;
        }
        // Every seq validated — commit the batch. No partial state was observable on the reject path.
        for (const EventRecord& r : batch) e.log.push_back(r);
        e.last_seq = prev;
        return {};
    }

    [[nodiscard]] result<EventCursor> read_log(ActorId id, SeqNo from) const {
        EventCursor cur;
        const auto it = table_.find(id);
        if (it == table_.end()) return cur;
        for (const auto& ev : it->second.log)
            if (ev.seq >= from) cur.entries.push_back(ev);
        return cur;
    }

    // --- Test/diagnostic introspection (not part of the Store seam) ---------------------------
    [[nodiscard]] std::size_t log_size(ActorId id) const noexcept {
        const auto it = table_.find(id);
        return it == table_.end() ? 0 : it->second.log.size();
    }
    [[nodiscard]] FenceToken current_owner(ActorId id) const noexcept {
        const auto it = table_.find(id);
        return it == table_.end() ? FenceToken{0} : it->second.owner;
    }

private:
    struct Entry {
        FenceToken owner{};                  // current fencing owner (highest issued token)
        SeqNo last_seq = 0;                  // highest committed sequence
        std::optional<SnapshotRecord> snapshot;
        std::vector<EventRecord> log;        // append-only, seq-ordered
    };
    std::unordered_map<ActorId, Entry> table_;
};

static_assert(Store<InMemoryStore>, "InMemoryStore must model the 012 Store seam");

}  // namespace quark
