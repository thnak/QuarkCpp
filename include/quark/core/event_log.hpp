// Implements 012-Persistence §"Two models" (EventSourced) + §"Interaction with the single-executor
// invariant" + ADR-009 C7 (the EventSourced staging fence) + §"Recovery" (tail replay). An
// EventSourced actor stores an append-only log of state-changing events; recovery replays the log
// (from the last snapshot + tail) to reconstruct current state deterministically. Each committed
// event carries a monotonically increasing commit sequence number.
//
// STAGING FENCE (ADR-009 C7). Events raised by a handler STAGE in a per-message buffer and become
// durable only at the HANDLER-COMPLETION COMMIT POINT (015, on the single lane → naturally
// serialized). A throwing handler commits NOTHING: `rollback()` discards the staged buffer, so a
// subsequent Restart → reload replays only pre-poison committed state and can never resurrect a
// poison handler's partial append. Proven shape: stage debit(100) then throw → durable log empty,
// reconstructed balance 0; normal path → commit → log [100].
//
// COMMIT HOOK (reported seam). The commit/rollback split is meant to fire from the engine at handler
// completion inside `Activation::drain_step` — on successful return `log.commit()`, on a throw
// `log.rollback()`. That hook lives in activation.hpp (a core header this module must NOT edit), so
// this header exposes explicit `commit()`/`rollback()` that a handler (or a test) drives, and the
// exact hook is REPORTED rather than wired. On the single lane commits are naturally serialized, so
// each takes the next strictly-increasing sequence with no cross-lane coordination.
//
// This header owns ONLY the event-log staging/commit/replay mechanism. Snapshot serialize/recover is
// in `snapshot.hpp`; the storage seam + fencing token is in `persistence.hpp`.
#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "quark/core/describe.hpp"     // Described (016)
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/persistence.hpp"  // Store, FenceToken, SeqNo, EventCursor
#include "quark/core/serialize.hpp"    // read_migrated (replay migrates old event shapes)
#include "quark/core/snapshot.hpp"     // encode_durable, decode_durable_migrated, load_snapshot_migrated

namespace quark {

// ============================================================================================
// EventLog — the per-actor staging buffer + commit gate (ADR-009 C7). Held by the actor (or driven
// by a test). `stage()` buffers an event WITHOUT durability; `commit()` is the handler-completion
// point that appends every staged event under the fence, each with the next commit sequence;
// `rollback()` is the throwing-handler path that discards the buffer so nothing becomes durable.
// ============================================================================================
template <Described Event, Store S>
class EventLog {
public:
    // `next_seq` is the sequence the next commit will assign — a recovering actor passes
    // `store.last_seq(id) + 1` so the count continues strictly increasing across restarts.
    EventLog(S& store, ActorId id, FenceToken fence, SeqNo next_seq) noexcept
        : store_(&store), id_(id), fence_(fence), next_seq_(next_seq) {}

    // Raise an event within a handler — buffered in the per-message staging area, NOT yet durable.
    void stage(Event ev) { staged_.push_back(std::move(ev)); }

    [[nodiscard]] std::size_t staged_count() const noexcept { return staged_.size(); }
    [[nodiscard]] SeqNo next_seq() const noexcept { return next_seq_; }
    [[nodiscard]] FenceToken fence() const noexcept { return fence_; }

    // HANDLER-COMPLETION COMMIT (ADR-009 C7) — ALL-OR-NOTHING. Encode every staged event and assign
    // each the next strictly-increasing commit sequence into a batch built ENTIRELY BEFORE the store
    // is touched, then hand the batch to the store's ATOMIC `append_batch` in a SINGLE call. Returns
    // the last sequence committed (== previous next_seq-1 when the buffer was empty).
    //
    // On ANY failure (encode error, fence rejection, or the store failing the batch) NOTHING becomes
    // durable, `next_seq_` stays at the store tail, and the staging buffer is left INTACT so the
    // caller can retry once the store recovers. This closes the torn-commit hole: a non-first append
    // can no longer make earlier events durable while the batch as a whole fails (which regressed
    // next_seq_ behind the store tail and wedged the log). Because the whole commit is one atomic
    // batch, a throwing handler that never reaches commit — or a commit that fails — leaves the log
    // exactly as it was (ADR-009 C7: a failed/throwing handler commits NOTHING).
    [[nodiscard]] result<SeqNo> commit() {
        if (staged_.empty()) return next_seq_ == 0 ? SeqNo{0} : next_seq_ - 1;
        std::vector<EventRecord> batch;
        batch.reserve(staged_.size());
        SeqNo seq = next_seq_;
        for (const Event& ev : staged_) {
            auto bytes = encode_durable(ev);
            if (!bytes) return std::unexpected(bytes.error());  // nothing appended yet — retryable
            batch.push_back(EventRecord{seq, std::move(*bytes)});
            ++seq;
        }
        auto rc = store_->append_batch(id_, fence_, std::span<const EventRecord>(batch));
        if (!rc) return std::unexpected(rc.error());  // atomic: NONE durable — next_seq_/staged_ intact
        next_seq_ = seq;    // durable: advance past the batch
        staged_.clear();    // and clear staging only now
        return seq - 1;
    }

    // THROWING-HANDLER PATH (ADR-009 C7). Discard the staging buffer — commit nothing. Nothing this
    // handler staged ever reaches the store, so replay cannot resurrect a partial pre-throw append.
    void rollback() noexcept { staged_.clear(); }

private:
    S* store_;
    ActorId id_;
    FenceToken fence_;
    SeqNo next_seq_;
    // INTERIM: this is ONE staging buffer PER ACTOR, not per in-flight handler. It is correct for a
    // Sequential actor (at most one handler in flight, so at most one un-committed batch), which is
    // why `Persistent<EventSourced>` is forbidden at COMPILE TIME on Reentrant / MaxConcurrency<N>
    // actors (the static_assert in policies.hpp `validate_actor_policies`). On a reentrant actor two
    // handlers would share this buffer: B's commit would flush A's still-staged events and A's later
    // rollback would be a no-op → ADR-009 C7 ("a throwing handler commits nothing") violated.
    //
    // DEFERRED (proper Reentrant EventSourced per 012/ADR-009): staging must become PER-IN-FLIGHT-
    // HANDLER, with the commit SEQUENCE allocated at commit time (not at stage time). The sequence
    // allocator stays per-actor; only the staging buffer moves per-handler. That redesign is NOT
    // attempted here — the compile-time guard is the safe interim.
    std::vector<Event> staged_;  // per-ACTOR staging buffer (durable only at commit); see note above
};

// ============================================================================================
// Recovery (012 §Recovery) — reconstruct current state deterministically. Load the latest snapshot
// (compaction checkpoint), then replay the tail log (events after the snapshot's through_seq),
// folding each event into state via the domain `apply`. Old event shapes are migrated forward on
// read (016). Recovery is part of activation, not a message.
// ============================================================================================

// Replay the tail log into `state`: read every event with `seq > from`, decode (migrating an older
// schema forward through [OldestEvent … Event]), and fold it in with `apply(State&, const Event&)`.
// Returns the highest sequence applied (or `from` when the tail is empty). Deterministic: the fold
// order is the store's strictly-increasing seq order.
template <class Event, class OldestEvent = Event, class State, Store S, class Apply>
[[nodiscard]] result<SeqNo> replay_tail(S& store, ActorId id, SeqNo from, State& state,
                                        Apply&& apply) {
    auto cur = store.read_log(id, from + 1);
    if (!cur) return std::unexpected(cur.error());
    SeqNo last = from;
    for (const EventRecord& rec : *cur) {
        auto ev = read_migrated<Event, OldestEvent>(rec.record.data(), rec.record.size());
        if (!ev) return std::unexpected(ev.error());
        apply(state, *ev);
        last = rec.seq;
    }
    return last;
}

// Full EventSourced recovery: latest snapshot (migrated) → tail replay (migrated) → reconstructed
// state. `initial` seeds the fold when no snapshot exists yet. `apply` folds each event into state.
// Template chain heads (`OldestState`/`OldestEvent`) default to the current types (no evolution).
template <Described State, Described Event, class OldestState = State, class OldestEvent = Event,
          Store S, class Apply>
[[nodiscard]] result<RecoveredState<State>> recover_event_sourced(S& store, ActorId id,
                                                                  State initial, Apply&& apply) {
    RecoveredState<State> out;
    SeqNo from = 0;
    auto snap = load_snapshot_migrated<State, OldestState>(store, id);
    if (!snap) return std::unexpected(snap.error());
    if (snap->has_value()) {
        out.state = std::move((*snap)->state);
        out.fence = (*snap)->fence;
        from = (*snap)->through_seq;
    } else {
        out.state = std::move(initial);
    }
    auto last = replay_tail<Event, OldestEvent>(store, id, from, out.state, std::forward<Apply>(apply));
    if (!last) return std::unexpected(last.error());
    out.last_seq = *last;
    return out;
}

}  // namespace quark
