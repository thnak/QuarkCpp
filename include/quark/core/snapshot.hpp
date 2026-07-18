// Implements 012-Persistence §"Two models" (Snapshot) + §"Recovery" — the point-in-time state
// capture. A snapshot is the latest serialized state, written as a 016 canonical tagged durable
// record (header {type_key, schema_version} + body) so old bytes stay readable through the 016
// migration chain. Recovery = load the last snapshot, decode (migrate if older), reconstruct.
//
// CONSISTENT POINT (012 §"Interaction with the single-executor invariant"). A snapshot must capture
// a coherent state, so it is taken at a QUIESCENT point reached via `quiesce(Drain)` (015): admission
// is sealed and in-flight handlers are awaited to completion, after which the actor state is not
// being mutated. On a Sequential actor the actor is ALWAYS at a quiescent point between messages, so
// `quiesce(Drain)` resolves SYNCHRONOUSLY (`begin_quiesce` returns true) and the guard is a no-op —
// `snapshot_sequential` drives exactly that ready path. A Reentrant actor must `co_await
// act.quiesce(QuiesceMode::Drain)` inside a handler (the guard may suspend until in-flight drains);
// `snapshot_sequential` refuses (errc::unavailable) rather than serialize a torn state.
//
// This header owns ONLY the snapshot serialize/persist/recover mechanism. The event-sourced log is
// in `event_log.hpp`; the storage seam + fencing is in `persistence.hpp`.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "quark/core/activation.hpp"   // Activation, QuiesceMode, QuiescenceGuard (015 consistent point)
#include "quark/core/describe.hpp"     // Described, fingerprint_v (016)
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"  // Store, SnapshotRecord, FenceToken, SeqNo
#include "quark/core/serialize.hpp"    // encode_record, decode_record, read_migrated, durable header
#include "quark/core/wire.hpp"         // detail::tagged_object_size (buffer sizing)

namespace quark {

// The durable {type_key} carried in a snapshot/event record header. For a Described value type this
// is its 016 fingerprint — identical to the wire/durable key 008/016 already use — so no metadata
// registry dependency is pulled into the persistence path.
template <Described T>
[[nodiscard]] constexpr TypeKey durable_type_key() noexcept {
    return TypeKey{fingerprint_v<T>};
}

// Encode a Described value to a 016 canonical tagged DURABLE record (header {type_key,
// schema_version} + tagged body). Sizes the buffer exactly from the reflection-free describe pass,
// so there is no over-allocation and no truncation. Off the hot path (recovery / commit / snapshot).
template <Described T>
[[nodiscard]] result<std::vector<std::byte>> encode_durable(const T& value) {
    const std::size_t need = durable_header_size + detail::tagged_object_size(value);
    std::vector<std::byte> out(need);
    auto n = encode_record(value, durable_type_key<T>(), out.data(), out.size());
    if (!n) return std::unexpected(n.error());
    out.resize(*n);
    return out;
}

// Decode a durable record whose body is the CURRENT schema of `T` (no migration needed).
template <Described T>
[[nodiscard]] result<T> decode_durable(std::span<const std::byte> bytes) {
    return decode_record<T>(bytes.data(), bytes.size());
}

// Decode a durable record of ANY schema version in the migration chain [Oldest … Current] and fold
// it forward to `Current` (016 §"Migrations"). `Oldest` defaults to `Current` (no evolution yet).
template <class Current, class Oldest = Current>
[[nodiscard]] result<Current> decode_durable_migrated(std::span<const std::byte> bytes) {
    return read_migrated<Current, Oldest>(bytes.data(), bytes.size());
}

// ============================================================================================
// Snapshot write — serialize state and persist it under a fencing token (012 §Snapshot).
// ============================================================================================

// Serialize `state` and save it as the actor's latest snapshot, subsuming the log prefix up to
// `through_seq`. The store gate rejects the write if `fence` is stale (superseded writer). CALLER
// MUST hold a consistent point (a `QuiescenceGuard` from `quiesce(Drain)`); `snapshot_sequential`
// is the convenience that reaches it on a Sequential actor.
template <Described State, Store S>
[[nodiscard]] result<void> save_snapshot(S& store, ActorId id, FenceToken fence, SeqNo through_seq,
                                         const State& state) {
    auto bytes = encode_durable(state);
    if (!bytes) return std::unexpected(bytes.error());
    SnapshotRecord rec;
    rec.fence = fence;
    rec.through_seq = through_seq;
    rec.record = std::move(*bytes);
    return store.save_snapshot(id, fence, rec);
}

// Sequential consistent-point capture: reach quiescence via `quiesce(Drain)` — which resolves
// synchronously on a Sequential actor — then serialize + persist. Returns the `through_seq` written.
// For a Reentrant actor whose quiescence would SUSPEND (in-flight handlers still running), this
// refuses with errc::unavailable rather than snapshot a torn state; such actors capture from inside
// a handler with `co_await act.quiesce(QuiesceMode::Drain)`.
template <Described State, Store S>
[[nodiscard]] result<void> snapshot_sequential(Activation& act, S& store, ActorId id,
                                               FenceToken fence, SeqNo through_seq,
                                               const State& state) {
    auto aw = act.quiesce(QuiesceMode::Drain);
    if (!aw.await_ready()) {
        // Reentrant with live in-flight handlers — a synchronous snapshot here would be torn.
        return fail(errc::unavailable, "snapshot_sequential: actor not at a synchronous quiescent point");
    }
    QuiescenceGuard guard = aw.await_resume();  // holds the consistent point until end of scope
    (void)guard;
    return save_snapshot<State>(store, id, fence, through_seq, state);
}

// ============================================================================================
// Snapshot recovery — load the latest snapshot and decode it (012 §Recovery). Returns nullopt when
// the actor has never been snapshotted (a fresh actor reconstructs from its factory instead).
// ============================================================================================

// A recovered snapshot: the decoded state plus the commit sequence it subsumes (the point from
// which an EventSourced tail replay resumes).
template <class State>
struct RecoveredSnapshot {
    State state{};
    SeqNo through_seq = 0;
    FenceToken fence{};
};

// Load + decode the latest snapshot (current schema). nullopt ⇒ no snapshot on record.
template <Described State, Store S>
[[nodiscard]] result<std::optional<RecoveredSnapshot<State>>> load_snapshot(S& store, ActorId id) {
    auto raw = store.load_snapshot(id);
    if (!raw) return std::unexpected(raw.error());
    if (!raw->has_value()) return std::optional<RecoveredSnapshot<State>>{};
    const SnapshotRecord& rec = **raw;
    auto st = decode_durable<State>(rec.record);
    if (!st) return std::unexpected(st.error());
    return std::optional<RecoveredSnapshot<State>>{
        RecoveredSnapshot<State>{std::move(*st), rec.through_seq, rec.fence}};
}

// Load + decode the latest snapshot, migrating an older schema forward through the chain
// [Oldest … State] (016). Same nullopt contract.
template <class State, class Oldest, Store S>
[[nodiscard]] result<std::optional<RecoveredSnapshot<State>>> load_snapshot_migrated(S& store,
                                                                                     ActorId id) {
    auto raw = store.load_snapshot(id);
    if (!raw) return std::unexpected(raw.error());
    if (!raw->has_value()) return std::optional<RecoveredSnapshot<State>>{};
    const SnapshotRecord& rec = **raw;
    auto st = decode_durable_migrated<State, Oldest>(rec.record);
    if (!st) return std::unexpected(st.error());
    return std::optional<RecoveredSnapshot<State>>{
        RecoveredSnapshot<State>{std::move(*st), rec.through_seq, rec.fence}};
}

// The reconstructed state and the highest committed sequence seen (012 §Recovery). Shared by the
// Snapshot model (`recover_snapshot` here) and the EventSourced model (`recover_event_sourced` in
// event_log.hpp): a recovering actor resumes its log at `last_seq + 1`.
template <class State>
struct RecoveredState {
    State state{};
    SeqNo last_seq = 0;
    FenceToken fence{};  // the fence the snapshot was written under (0 if never snapshotted)
};

// Snapshot-model recovery (012 §Recovery, no event tail): load the latest snapshot, or seed with
// `initial` when the actor has never been snapshotted.
template <Described State, class OldestState = State, Store S>
[[nodiscard]] result<RecoveredState<State>> recover_snapshot(S& store, ActorId id, State initial) {
    RecoveredState<State> out;
    auto snap = load_snapshot_migrated<State, OldestState>(store, id);
    if (!snap) return std::unexpected(snap.error());
    if (snap->has_value()) {
        out.state = std::move((*snap)->state);
        out.fence = (*snap)->fence;
        out.last_seq = (*snap)->through_seq;
    } else {
        out.state = std::move(initial);
    }
    return out;
}

}  // namespace quark
