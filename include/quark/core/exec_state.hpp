// Implements 001-Actor-Execution-Model §Lifecycle + ADR-015 §exec-state machine — the per-actor
// execution state and the atomic transition protocol that enforces the single-executor invariant.
//
//   Idle → Scheduled → Running → (Idle | Scheduled | Parked)
//
// The transitions are the load-bearing memory edges (001 §Mailbox, ADR-002/003/004): the SAME
// atomic carries two obligations with different orders — the consumer-private `head_` handoff
// (release on relinquish, acquire on claim), and the Dekker StoreLoad wakeup rendezvous with
// producers. ADR-066: the rendezvous is spelled as seq_cst OPERATIONS on both sides (the `_sc`
// relinquish stores below + the producer's seq_cst wake CAS), not as a standalone fence.
//
// `Parked` (ADR-015) is a SEALED state that FAILS every admission CAS — while an actor is parked
// on an in-flight async/blocking/fiber call no worker may claim it; single-executor is preserved
// by the seal, not by luck (a `-DQUARK_PARK_IDLE=1`-style park-as-Idle double-executes, ADR-015).
#pragma once

#include <atomic>
#include <cstdint>

namespace quark {

enum class ExecState : std::uint8_t {
    Idle = 0,       // no pending work, no activation queued
    Scheduled = 1,  // an activation is queued in exactly one shard run-queue
    Running = 2,    // a worker holds execution ownership and is draining
    Parked = 3,     // sealed: an async/blocking/fiber leaf is in flight (ADR-015); admission fails
    Dormant = 4,    // sealed: evicted on idle-timeout (ADR-028 Phase 1); no live actor object right
                    // now — admission fails until a future reactivation path re-admits (Phase 4 seam).
    Retired = 5,    // sealed: unlinked from the id table, pending free (ADR-052 reclaim); only the
                    // owning shard's reclaim sweep leaves this state (Retired->Scheduled rescue, or free).
};

// The per-actor exec-state atomic + its transition protocol. All memory orders are load-bearing.
class ExecStateCell {
public:
    ExecStateCell() noexcept : s_(ExecState::Idle) {}

    ExecStateCell(const ExecStateCell&) = delete;
    ExecStateCell& operator=(const ExecStateCell&) = delete;

    [[nodiscard]] ExecState state() const noexcept { return s_.load(std::memory_order_acquire); }

    // --- Producer / wake edge -------------------------------------------------------------
    // Enqueue drives Idle → Scheduled; a `true` return is the "wake exactly one worker" edge
    // (002 targeted wakeup — the wake callback itself is a 002 seam). From Running the current
    // drain (or its close-out probe) will observe the message; from Scheduled it is already
    // pending; from Parked the completion re-admit will drain it — all return false (no wake).
    // This CAS is the producer's "load exec_state" half of the Dekker rendezvous (ADR-066): seq_cst on
    // BOTH the success and the failure path, so the value it reads — including a failed read of
    // Running/Dormant/Parked — is ordered in S after the caller's seq_cst enqueue exchange. Same
    // instruction as acq_rel on x86-64 (`lock cmpxchg`) and AArch64 (`casal` / LL-SC loop).
    [[nodiscard]] bool notify_enqueued() noexcept {
        ExecState expected = ExecState::Idle;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_seq_cst, std::memory_order_seq_cst);
    }

    // --- Worker acquire edges (acquire the head_ handoff published by the prior release) ----
    // Normal claim: Scheduled → Running. false ⇒ lost the race / not scheduled ⇒ worker skips.
    [[nodiscard]] bool try_acquire() noexcept {
        ExecState expected = ExecState::Scheduled;
        return s_.compare_exchange_strong(expected, ExecState::Running,
                                          std::memory_order_acquire, std::memory_order_relaxed);
    }
    // Close-out re-acquire: Idle → Running, after the read-only probe finds work (001 §Mailbox).
    [[nodiscard]] bool reacquire_from_idle() noexcept {
        ExecState expected = ExecState::Idle;
        return s_.compare_exchange_strong(expected, ExecState::Running,
                                          std::memory_order_acquire, std::memory_order_relaxed);
    }
    // Reentrant park close-out re-acquire: Parked → Running (ADR-015 §Reentrancy). The SAME Dekker
    // rendezvous as the Idle close-out but from the sealed Parked state — after a reentrant lane
    // stores Parked (in-flight frames remain) and the seq_cst probe finds a carrier-completed frame
    // (or an admissible mailbox message), the lane re-claims ownership here. false ⇒ a carrier won
    // the race and already re-admitted (Parked → Scheduled); it will wake a worker.
    [[nodiscard]] bool reacquire_from_parked() noexcept {
        ExecState expected = ExecState::Parked;
        return s_.compare_exchange_strong(expected, ExecState::Running,
                                          std::memory_order_acquire, std::memory_order_relaxed);
    }
    // Deactivate close-out re-acquire: Dormant → Running (ADR-028 Phase 1). The SAME Dekker
    // rendezvous as the Idle/Parked close-outs — after the lane tentatively stores Dormant and the
    // seq_cst probe finds a message raced in, the lane re-claims ownership here (the eviction is
    // aborted, the race message dispatches). false ⇒ a future reactivation path already won the race
    // (Dormant → Scheduled) and will wake a worker (Phase 4 seam; nothing does this yet).
    [[nodiscard]] bool reacquire_from_dormant() noexcept {
        ExecState expected = ExecState::Dormant;
        return s_.compare_exchange_strong(expected, ExecState::Running,
                                          std::memory_order_acquire, std::memory_order_relaxed);
    }

    // --- Relinquish edges (release: publish the consumer-private head_ for the next worker) --
    void release_to_idle() noexcept { s_.store(ExecState::Idle, std::memory_order_release); }
    void yield_to_scheduled() noexcept { s_.store(ExecState::Scheduled, std::memory_order_release); }
    // Running → Parked (ADR-015): seals the actor while an async/blocking/fiber leaf is in flight.
    void park() noexcept { s_.store(ExecState::Parked, std::memory_order_release); }

    // --- Dekker close-out relinquish edges (002 §Release close-out; ADR-066) ----------------
    // The STORE half of the close-out StoreLoad rendezvous: a seq_cst store, followed by the caller's
    // seq_cst work probe. Both sit in the single total order S, as do the producer's seq_cst enqueue
    // exchange and wake CAS, so "consumer misses the message AND producer misses the relinquish" is
    // forbidden by the language model itself (C++20 [atomics.order]) — no standalone fence, which
    // ThreadSanitizer cannot model. A seq_cst store is still a release, so the head_ handoff above is
    // unchanged. Lowering: x86-64 `xchg` (the one locked instruction the former `mov` + fence pair
    // already paid); AArch64 a bare `stlr`, ordered before the probe's `ldar` (the `dmb ish` is gone).
    void release_to_idle_sc() noexcept { s_.store(ExecState::Idle, std::memory_order_seq_cst); }
    void park_sc() noexcept { s_.store(ExecState::Parked, std::memory_order_seq_cst); }
    // Running → Dormant (ADR-028 Phase 1): tentatively seals the actor for eviction. Its ONLY use is
    // the deactivate close-out, so it is always the seq_cst Dekker store (same as release_to_idle_sc),
    // followed by the same seq_cst probe before the caller commits to freeing the actor instance.
    void retire_to_dormant() noexcept { s_.store(ExecState::Dormant, std::memory_order_seq_cst); }

    // --- 015 re-admit: Parked → Scheduled. The completion is a structurally new third-party
    // waker (carrier → actor); it carries the same seq_cst Dekker rendezvous as the close-out but
    // is a DISTINCT StoreLoad pair (002 §Blocking/fiber adapter completion). true ⇒ should wake.
    // seq_cst on both paths for the same reason as notify_enqueued (ADR-066) — same instructions.
    [[nodiscard]] bool readmit_from_parked() noexcept {
        ExecState expected = ExecState::Parked;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_seq_cst, std::memory_order_seq_cst);
    }
    // ADR-028 Phase 4 seam (unused until the broker lands): Dormant → Scheduled, the edge a future
    // reactivation path uses to re-admit an evicted activation once its actor storage is rebuilt.
    // Same shape as readmit_from_parked() — a structurally new third-party waker (broker → actor).
    // Also the producer's Dormant fallback in Activation::wake_after_enqueue (a Dekker load half).
    [[nodiscard]] bool readmit_from_dormant() noexcept {
        ExecState expected = ExecState::Dormant;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_seq_cst, std::memory_order_seq_cst);
    }

    // ADR-052: Dormant -> Retired (sweep, drain-owner only). CAS so a racing readmit wins cleanly.
    [[nodiscard]] bool retire_from_dormant() noexcept {
        ExecState expected = ExecState::Dormant;
        return s_.compare_exchange_strong(expected, ExecState::Retired,
                                          std::memory_order_acq_rel, std::memory_order_acquire);
    }
    // ADR-052: Retired -> Scheduled (rescue: a stale-pointer producer stranded a message).
    [[nodiscard]] bool readmit_from_retired() noexcept {
        ExecState expected = ExecState::Retired;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    std::atomic<ExecState> s_;
};

static_assert(std::atomic<ExecState>::is_always_lock_free, "exec-state atomic must be lock-free");

}  // namespace quark
