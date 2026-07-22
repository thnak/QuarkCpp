// Implements 001-Actor-Execution-Model §Lifecycle + ADR-015 §exec-state machine — the per-actor
// execution state and the atomic transition protocol that enforces the single-executor invariant.
//
//   Idle → Scheduled → Running → (Idle | Scheduled | Parked)
//
// The transitions are the load-bearing memory edges (001 §Mailbox, ADR-002/003/004): the SAME
// atomic carries two obligations with different orders — the consumer-private `head_` handoff
// (release on relinquish, acquire on claim), and the Dekker StoreLoad wakeup rendezvous with
// producers (the seq_cst fence lives in the mailbox close-out seam; see Activation::close_out).
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
    // This CAS is the producer's "load exec_state" half of the Dekker rendezvous; the caller must
    // have issued the producer-side StoreLoad fence after enqueue (elided on x86-TSO).
    [[nodiscard]] bool notify_enqueued() noexcept {
        ExecState expected = ExecState::Idle;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_acq_rel, std::memory_order_acquire);
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
    // Running → Dormant (ADR-028 Phase 1): tentatively seals the actor for eviction. Paired with the
    // SAME seq_cst close-out fence + probe as every other relinquish edge (see Activation::close_out)
    // before the caller commits to actually freeing the actor instance.
    void retire_to_dormant() noexcept { s_.store(ExecState::Dormant, std::memory_order_release); }

    // --- 015 re-admit: Parked → Scheduled. The completion is a structurally new third-party
    // waker (carrier → actor); it carries the same seq_cst Dekker rendezvous as the close-out but
    // is a DISTINCT StoreLoad pair (002 §Blocking/fiber adapter completion). true ⇒ should wake.
    [[nodiscard]] bool readmit_from_parked() noexcept {
        ExecState expected = ExecState::Parked;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_acq_rel, std::memory_order_acquire);
    }
    // ADR-028 Phase 4 seam (unused until the broker lands): Dormant → Scheduled, the edge a future
    // reactivation path uses to re-admit an evicted activation once its actor storage is rebuilt.
    // Same shape as readmit_from_parked() — a structurally new third-party waker (broker → actor).
    [[nodiscard]] bool readmit_from_dormant() noexcept {
        ExecState expected = ExecState::Dormant;
        return s_.compare_exchange_strong(expected, ExecState::Scheduled,
                                          std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    std::atomic<ExecState> s_;
};

static_assert(std::atomic<ExecState>::is_always_lock_free, "exec-state atomic must be lock-free");

}  // namespace quark
