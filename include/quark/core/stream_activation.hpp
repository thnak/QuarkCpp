// Implements 024-Streaming-and-Inbound-Streams §Arming/wakeup/un-stall + §Developer surface +
// §How it composes (001/002/015), and reproduces the ADR-014 promotion gate (async-suspend /
// resume against the REAL 002 exec-state machine + the 015 admission gate — exactly-once across
// suspension, transferred-not-parked, no descriptor double-enqueue/orphan).
//
// This header COMPOSES the settled primitives VERBATIM — it forks NOTHING:
//   * the `StreamChannel<F>` credit-ring (stream_channel.hpp) sits OFF the mailbox;
//   * the scheduling rides the Accepted 002/015 `ExecStateCell` (exec_state.hpp) — Idle -> Scheduled
//     -> Running -> Parked, `notify_enqueued`, `try_acquire`, `park`, `readmit_from_parked`,
//     `reacquire_from_idle` — the SAME class the Engine drains actors with;
//   * the wakeup + relinquish ride the mailbox `seq_cst` Dekker close-out fences
//     (`Mailbox::consumer_close_out_fence` / `producer_close_out_fence`) VERBATIM.
// The ordinary Vyukov mailbox + the discrete tell/ask hot path are BYTE-FOR-BYTE UNCHANGED (this is
// a separate, additively-included header; no core file is edited). A non-streaming actor constructs
// no StreamChannel/StreamActivation at all (024 §zero-cost-when-unused).
//
// THE EXACTLY-ONCE-ACROSS-SUSPEND SEAM (024 §Why disp and tail are split; ADR-014):
//   * dispatch of frame k: `disp`->k+1, `tail` stays  (the frame is dispatched, credit NOT returned);
//   * SUSPEND: the activation goes Running -> Parked (sealed, single-executor); the window is pinned
//     at the parked frame — the producer's derived credit `window-(head-tail)` provably cannot cover
//     the parked slot, so the producer cannot overwrite it;
//   * COMPLETION (a carrier on a FOREIGN thread): a Parked -> Scheduled TRANSFER via
//     `readmit_from_parked` — the reusable readiness is NOT re-posted (no descriptor re-enqueue /
//     orphan). `tail` (hence credit) advances ONLY when a worker RE-ACQUIRES the exec-state CAS
//     afresh and observes the completion on the actor's lane (`resume_if_ready`), so credit is
//     returned ONLY for COMPLETED frames (0 for parked-not-completed).
//
// SEAMS LEFT EXPLICIT (named downstream owner — see stream_channel.hpp for the ring-level list):
//   * Full `Engine::run_activation` routing of the reusable stream descriptor through the worker
//     loop  ->  a small ADDITIVE 002 seam (this header composes ExecStateCell directly, exactly as
//     the exec_single_executor / reentrancy harnesses drive an Activation lane without the Engine);
//   * BlockingHandler / stackful `quark::fiber<>` C4 multiplexing  ->  ADR-015 (the transferred-not-
//     parked re-admission is what IS implemented here; the stackful multiplexing is the seam);
//   * `system.open_stream<F>(actor_id, transport_ep)` addressing/transport binding  ->  006/010.
//   * ARM64 weak-memory re-gate of the `armed.exchange`-as-Dekker-arm + the close-out fences  ->
//     deferred (x86-TSO proven only). TODO(arm64): re-gate under weak memory (herd7/GenMC).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <utility>

#include "quark/core/config.hpp"
#include "quark/core/error.hpp"
#include "quark/core/exec_state.hpp"
#include "quark/core/mailbox.hpp"          // the seq_cst Dekker close-out fences (reused verbatim)
#include "quark/core/stream_channel.hpp"

namespace quark {

// The per-frame processing verdict a stream handler returns (024 §Developer surface). `Completed`
// runs to completion inline on the lane (credit returns immediately); `Suspended` parks the
// activation (a `co_await` / BlockingHandler leaf) — credit is pinned until the 015 completion.
enum class FrameVerdict : std::uint8_t { Completed = 0, Suspended = 1 };

// The relinquish decision of one drain turn (mirrors Activation::DrainOutcome, stream flavor).
enum class StreamDrainOutcome : std::uint8_t {
    DrainedEmpty,     // occupancy 0, nothing parked -> close_out to Idle
    BudgetExhausted,  // drain budget spent with work remaining -> yield Running->Scheduled (fairness)
    Suspended,        // a handler parked the activation (Running->Parked); a carrier will re-admit
};

template <class F>
class StreamActivation;

// ============================================================================================
// StreamRef<F> — the developer handle to an open stream AND the single-writer token (024 §Developer
// surface / §Single-producer precondition). Move-only: a stream has exactly ONE producer; a second
// bind is a typed 007 error (StreamActivation::bind_producer). `try_push` is lossless backpressure
// (false == credit depleted -> stall); `push_blocking` stalls-and-waits on the credit-return edge.
// ============================================================================================
template <class F>
class StreamRef {
public:
    StreamRef() noexcept = default;

    StreamRef(const StreamRef&) = delete;
    StreamRef& operator=(const StreamRef&) = delete;
    StreamRef(StreamRef&& o) noexcept : act_(std::exchange(o.act_, nullptr)) {}
    StreamRef& operator=(StreamRef&& o) noexcept {
        if (this != &o) act_ = std::exchange(o.act_, nullptr);
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return act_ != nullptr; }

    // Non-blocking, lossless. false == credit depleted (the producer must back off / stall).
    [[nodiscard]] bool try_push(const F& frame) noexcept { return act_->producer_push(frame); }

    // Blocking, lossless: stall on credit depletion and wait on the reverse-Dekker credit-return edge
    // (024 §Producer un-stall). Never sheds a frame. Off the hot path (only under sustained overproduce).
    // Rides the arm-edge notify so a fresh empty->nonempty transition still schedules the drain.
    void push_blocking(const F& frame) noexcept { act_->producer_push_blocking(frame); }

private:
    friend class StreamActivation<F>;
    explicit StreamRef(StreamActivation<F>* a) noexcept : act_(a) {}
    StreamActivation<F>* act_ = nullptr;
};

// ============================================================================================
// StreamBatch<F> — the drain-side view handed to a SYNC batch handler `handle(StreamBatch<F>&)`
// (024 §Developer surface). One drain turn: `next()` dispatches (advances `disp`) the next frame or
// returns nullptr at the batch end (occupancy 0 or budget spent); `retire()` returns the last
// frame's credit (advances `tail`). The whole loop is plain acquire-load + release-store — 0 RMW.
// (The suspend-capable per-frame drive lives in StreamActivation::drain; this is the sync surface.)
// ============================================================================================
template <class F>
class StreamBatch {
public:
    StreamBatch(StreamChannel<F>& ch, std::uint32_t budget) noexcept : ch_(&ch), remaining_(budget) {}

    [[nodiscard]] const F* next() noexcept {
        if (remaining_ == 0 || ch_->occupancy() == 0) return nullptr;
        const F* f = &ch_->peek();
        last_idx_ = ch_->advance_dispatch();  // DISPATCH edge (release store)
        --remaining_;
        ++dispatched_;
        return f;  // the slot is pinned until retire() (tail has not advanced past it)
    }
    void retire() noexcept {
        ch_->advance_tail();  // CREDIT-RETURN edge (release store)
        ++retired_;
    }

    [[nodiscard]] std::uint32_t dispatched() const noexcept { return dispatched_; }
    [[nodiscard]] std::uint32_t retired() const noexcept { return retired_; }
    [[nodiscard]] std::uint64_t last_index() const noexcept { return last_idx_; }

private:
    StreamChannel<F>* ch_;
    std::uint32_t remaining_;
    std::uint32_t dispatched_ = 0;
    std::uint32_t retired_ = 0;
    std::uint64_t last_idx_ = 0;
};

// ============================================================================================
// StreamActivation<F> — the ring + the exec-state-wired drive. Owns a StreamChannel<F> and an
// ExecStateCell; provides the producer arm-edge, the worker drain (suspend-capable), the carrier
// completion transfer, and the seq_cst Dekker close-out — all composing the settled 002/015
// primitives. Non-movable (holds atomics + a StreamChannel). Instrumented for the ADR-014 oracles
// (descriptor membership, single-executor witness, credit-for-parked) at NO per-frame RMW cost —
// the counters ride the per-session exec-state CAS edges, never the per-frame drain loop.
// ============================================================================================
template <class F>
class StreamActivation {
public:
    using Config = typename StreamChannel<F>::Config;

    StreamActivation(const Config& cfg, std::pmr::memory_resource* mr) : ch_(cfg, mr) {}

    StreamActivation(const StreamActivation&) = delete;
    StreamActivation& operator=(const StreamActivation&) = delete;

    // ---- Single-producer token (024 §Single-producer precondition; ADR-005 residual risk 3) -------
    // The FIRST bind hands out the single-writer StreamRef; a SECOND bind is a typed 007 error plus a
    // debug assertion (multi-source fan-in must stay on the mailbox — SPSC precondition, GATE-5).
    [[nodiscard]] result<StreamRef<F>> bind_producer() noexcept {
        bool expected = false;
        if (!producer_bound_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed)) {
            return std::unexpected<error>(
                error{errc::unavailable, "stream already bound: single-producer precondition (024)"});
        }
        return StreamRef<F>{this};
    }

    [[nodiscard]] StreamChannel<F>& channel() noexcept { return ch_; }
    [[nodiscard]] const StreamChannel<F>& channel() const noexcept { return ch_; }

    // ======================================================================================
    // PRODUCER side — push a frame and, on the ring's empty->nonempty ARM-EDGE, post the reusable
    // readiness (drive Idle->Scheduled and wake a worker). `armed_.exchange(true)` is the SINGLE
    // cross-core RMW of the whole design and doubles as the producer's StoreLoad Dekker arm on x86
    // (lock xchg is a full barrier); it fires per arm-edge, NEVER per frame (024 §Arming; ADR-005 S1).
    // ======================================================================================
    [[nodiscard]] bool producer_push(const F& frame) noexcept {
        if (!ch_.try_push(frame)) return false;  // credit depleted -> lossless stall
        arm_edge();
        return true;
    }

    // Blocking, lossless producer push (024 §Producer un-stall): stall on credit depletion, wait on the
    // reverse-Dekker credit-return, then post the readiness on the arm-edge.
    void producer_push_blocking(const F& frame) noexcept {
        ch_.push_blocking(frame);  // blocks until credit; never sheds
        arm_edge();
    }

    // ======================================================================================
    // WORKER side (any lane; at-most-one at a time via the exec-state CAS).
    // ======================================================================================

    // Claim the activation: Scheduled -> Running. true == this worker owns the drain now.
    [[nodiscard]] bool try_acquire() noexcept {
        if (!exec_.try_acquire()) return false;
        leave_run_queue();  // instrumentation: took the descriptor out of the run-queue (membership--)
        running_enter();
        return true;
    }

    // On a FRESH Running acquisition, drain any completion the carrier transferred to us (015 gate):
    // run the parked frame's continuation ON THE LANE (never on the carrier), then advance `tail`
    // (credit for the now-COMPLETED frame). No-op when nothing is pending.
    template <class Processor>
    void resume_if_ready(Processor& proc) noexcept {
        if (ready_.exchange(false, std::memory_order_acquire)) {  // consume the completion (acquire)
            const std::uint64_t idx = parked_idx_;
            proc.on_resume(ch_.slot_at(idx), idx);  // continuation on the lane (transferred, not parked)
#ifndef QUARK_STREAM_SINGLE_CURSOR
            ch_.advance_tail();  // credit returns ONLY now that frame `idx` has COMPLETED
#endif
            parked_active_ = false;
        }
    }

    // Drain a batch. Plain acquire-load + release-store per frame — 0 cross-core RMW (023 gate). On a
    // suspending frame the activation parks and the drain returns immediately (the window pins there).
    template <class Processor>
    [[nodiscard]] StreamDrainOutcome drain(std::uint32_t budget, Processor& proc) noexcept {
        std::uint32_t n = 0;
        for (;;) {
            if (n >= budget) {
                ch_.poll_unstall();  // batch boundary: wake a credit-starved producer (off per-frame path)
                running_leave();
                return StreamDrainOutcome::BudgetExhausted;
            }
            if (ch_.occupancy() == 0) {
                ch_.poll_unstall();
                // running_leave() happens in close_out(); leave the Running region there.
                return StreamDrainOutcome::DrainedEmpty;
            }
            const std::uint64_t idx = ch_.advance_dispatch();  // DISPATCH edge: disp -> idx+1
#ifdef QUARK_STREAM_SINGLE_CURSOR
            // CONTROL-4 (single cursor): collapse disp/tail — return credit at DISPATCH, before the
            // handler completes. A suspended frame's slot is then credited and the producer overwrites
            // it (torn/lost) — the defect the split cursor exists to prevent (ADR-014 CONTROL-4).
            ch_.advance_tail();
#endif
            ++n;
            const FrameVerdict v = proc.on_frame(ch_.slot_at(idx), idx);
            if (v == FrameVerdict::Suspended) {
                parked_idx_ = idx;
                parked_active_ = true;
                // credit-for-parked oracle: with the split cursor `tail == idx` at park (the parked
                // frame is the oldest un-credited). A collapsed cursor advanced tail past it.
                if (ch_.tail() > idx) credit_for_parked_.fetch_add(1, std::memory_order_relaxed);
                running_leave();
                exec_.park();  // Running -> Parked (sealed): no other worker may enter (single-executor)
                return StreamDrainOutcome::Suspended;
            }
#ifndef QUARK_STREAM_SINGLE_CURSOR
            ch_.advance_tail();  // inline-completed frame -> return its credit now
#endif
        }
    }

    // Relinquish on DrainedEmpty: the seq_cst Dekker close-out (mailbox close-out reused verbatim).
    // NEVER keyed on ring emptiness alone — the wakeup rides the exec-state; the occupancy re-probe is
    // the Dekker step (024 §Arming: "never keyed on ring emptiness, which is non-linearizable").
    [[nodiscard]] bool close_out() noexcept {
        running_leave();                                // leave the drain region BEFORE relinquishing, so
                                                        // a worker that acquires in the close-out window is
                                                        // never (falsely) counted as a 2nd concurrent executor
        exec_.release_to_idle();                        // Running -> Idle (publishes disp/tail state)
        armed_.store(false, std::memory_order_release); // disarm: a plain release store — the consumer
                                                        // NEVER exchanges `armed_` (ADR-005: producer-only arm-RMW)
#ifndef QUARK_STREAM_NO_FENCE
        Mailbox::consumer_close_out_fence();            // seq_cst StoreLoad (consumer half of the Dekker)
#endif
        if (ch_.occupancy() > 0) {                      // work raced in during the close-out window
            if (exec_.reacquire_from_idle()) {          // Idle -> Running: we win, keep draining
                running_enter();                         // re-entered the drain region
                armed_.store(true, std::memory_order_release);  // we are the active readiness again
                return true;
            }
            // else a producer re-armed + posted; that worker drains it. We are done.
            return false;
        }
        return false;  // truly idle
    }

    // Fairness yield on BudgetExhausted: Running -> Scheduled (re-enter the run-queue; no wake — the
    // same/next worker re-acquires). Mirrors a budget-exhausted mailbox drain (024 §Arming).
    void yield_to_scheduled() noexcept {
        exec_.yield_to_scheduled();
        enter_run_queue();  // back in the run-queue (membership 0->1; a try_acquire will take it)
    }

    // ======================================================================================
    // CARRIER side (a FOREIGN thread completing the parked async op) — the 015 admission gate.
    // TRANSFER, not re-enqueue: publish the completion (`ready_`), then a single Parked -> Scheduled
    // CAS. The readiness descriptor is NOT re-posted; `tail` advances only when a worker re-acquires
    // and observes `ready_` (resume_if_ready). The `readmit` CAS is a full barrier on x86, ordering
    // the `ready_` release-store before the exec load (the completion's distinct StoreLoad pair).
    // ======================================================================================
    template <class Processor>
    [[nodiscard]] bool carrier_complete(Processor& proc) noexcept {
#ifdef QUARK_STREAM_REENQUEUE
        // CONTROL-5 (re-enqueue): on completion RE-ENQUEUE + resume INLINE on the carrier instead of
        // the clean transfer — a SECOND executor drains the parked frame while the lane also does, so
        // the frame is DOUBLE-DISPATCHED (dup>0) and the descriptor's membership exceeds 1 (ADR-014
        // CONTROL-5). This is the defect transfer-not-re-enqueue prevents.
        running_enter();                                            // a 2nd executor now runs
        ready_.store(true, std::memory_order_release);              // the lane will ALSO resume `idx` -> dup
        proc.on_resume(ch_.slot_at(parked_idx_), parked_idx_);      // carrier resumes it too
        ch_.advance_tail();
        running_leave();
        if (armed_.exchange(true, std::memory_order_acq_rel) == false)
            enter_run_queue();                                      // re-enqueue -> membership can exceed 1
        const bool wake = exec_.readmit_from_parked();
        if (wake) enter_run_queue();
        return wake;
#else
        (void)proc;  // the correct path resumes on the LANE (resume_if_ready), never on the carrier
        ready_.store(true, std::memory_order_release);  // publish the completion BEFORE the transfer CAS
        const bool wake = exec_.readmit_from_parked();  // Parked -> Scheduled (no descriptor re-post)
        if (wake) enter_run_queue();                    // the transferred readiness is in the run-queue
        return wake;
#endif
    }

    // ---- Observers / test seams -----------------------------------------------------------------
    [[nodiscard]] ExecState state() const noexcept { return exec_.state(); }
    [[nodiscard]] bool parked() const noexcept { return state() == ExecState::Parked; }
    [[nodiscard]] int max_descriptor_membership() const noexcept {
        return max_membership_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int max_running_executors() const noexcept {
        return max_running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t credit_for_parked() const noexcept {
        return credit_for_parked_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t tail() const noexcept { return ch_.tail(); }

private:
    // Run-queue membership instrumentation (the "armed-flag membership counter", ADR-014). ENTER on a
    // wake edge (Idle/Parked/Running -> Scheduled); LEAVE on try_acquire (Scheduled -> Running). Max
    // must be 1 — a re-enqueue bug pushes it past 1. All off the per-frame drain loop.
    void enter_run_queue() noexcept {
        const int m = membership_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = max_membership_.load(std::memory_order_relaxed);
        while (m > prev && !max_membership_.compare_exchange_weak(prev, m, std::memory_order_relaxed)) {}
    }
    void leave_run_queue() noexcept { membership_.fetch_sub(1, std::memory_order_acq_rel); }

    void running_enter() noexcept {
        const int r = running_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int prev = max_running_.load(std::memory_order_relaxed);
        while (r > prev && !max_running_.compare_exchange_weak(prev, r, std::memory_order_relaxed)) {}
    }
    void running_leave() noexcept { running_.fetch_sub(1, std::memory_order_acq_rel); }

    // The ring's empty->nonempty ARM-EDGE: post the reusable readiness exactly once per disarmed->armed
    // transition. `armed_.exchange(true)` is the single cross-core RMW of the design + the producer's
    // StoreLoad Dekker arm on x86 (lock xchg is a full barrier); fires per arm-edge, NEVER per frame.
    void arm_edge() noexcept {
        if (armed_.exchange(true, std::memory_order_acq_rel) == false) {
            if (exec_.notify_enqueued())  // Idle->Scheduled: the reusable descriptor enters the run-queue
                enter_run_queue();        // instrumentation: membership 0->1 (max must be 1)
        }
    }

    StreamChannel<F> ch_;
    QUARK_CACHE_ALIGNED ExecStateCell exec_{};        // the settled 002/015 state machine (verbatim)
    QUARK_CACHE_ALIGNED std::atomic<bool> armed_{false};  // reusable-descriptor membership / Dekker arm
    std::atomic<bool> ready_{false};                  // carrier -> lane completion publication
    std::atomic<bool> producer_bound_{false};         // single-writer token (024 §Single-producer)

    std::uint64_t parked_idx_ = 0;  // the disp index pinned while Parked (lane-private under the seal)
    bool parked_active_ = false;

    // Instrumentation (per-session CAS edges only; never per frame).
    std::atomic<int> membership_{0};
    std::atomic<int> max_membership_{0};
    std::atomic<int> running_{0};
    std::atomic<int> max_running_{0};
    std::atomic<std::uint64_t> credit_for_parked_{0};
};

// ============================================================================================
// open_stream<F>(activation) — the developer-facing open (024 §Developer surface). Binds the single
// producer and returns the StreamRef token. The `system.open_stream<F>(actor_id, transport_ep)` full
// form (resolving the actor + wiring the transport endpoint) is the 006/010 addressing seam; this is
// the std-only core it lowers onto.
// ============================================================================================
template <class F>
[[nodiscard]] inline result<StreamRef<F>> open_stream(StreamActivation<F>& act) noexcept {
    return act.bind_producer();
}

}  // namespace quark
