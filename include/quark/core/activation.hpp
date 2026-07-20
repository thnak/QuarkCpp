// Implements 001-Actor-Execution-Model §Activation + 015-Reentrancy-and-Quiescence — the single-lane
// execution core sitting directly on the 003 mailbox, EXTENDED with the 015 admission gate, the
// in-flight set, the reentrant frame pool, and the quiescence primitive (ADR-009).
//
// An Activation owns the per-actor Mailbox + exec-state atomic + actor instance pointer + dispatch
// table, and provides the run-to-completion drain step. It upholds the two hard invariants of
// 001/002 with NO per-message locking:
//   * single-executor — at most one worker owns an actor (the exec-state CAS, ADR-015 S1);
//   * mailbox FIFO + no-advance-past-suspended — a suspended async handler does not let a SIBLING's
//     *synchronous region* interleave (the split-executor rule, 001 §Hybrid handler execution).
//
// ============================================================================================
// TWO EXECUTION PATHS, SELECTED BY POLICY (015 §Zero cost for the common case)
//
//   * Sequential (max_concurrency == 1): the reentrant core (`rc_`) is NEVER allocated. `drain_step`
//     takes the ORIGINAL 001 single-in-flight park path VERBATIM — one predictable `if (rc_)` branch
//     guards entry, then the code below is byte-for-byte the pre-015 drain. `complete_parked()` is
//     the single-frame completion seam the engine (002) drives. Sequential pays nothing: no counter,
//     no waiter, no mutex, no frame pool (the counter-and-waiter machinery is only instantiated for
//     Reentrant / MaxConcurrency<N>).
//
//   * Reentrant / MaxConcurrency<N> (max_concurrency != 1): the reentrant core is cold-allocated at
//     construction. Multiple handlers may be SUSPENDED at once (up to the cap), but their
//     SYNCHRONOUS regions between `co_await` points run mutually exclusively on the actor's lane —
//     only the worker holding `Running` ever starts/resumes a frame, so there are NO data races on
//     actor state (015 §The execution model). A carrier that completes an in-flight async op does
//     NOT resume the frame inline: it hands the frame back through the 015 admission gate
//     (ready-queue + Parked→Scheduled re-admit), so the completing thread never becomes a second
//     executor (015 §The drain invariant holds across async completion).
//
// THE ADMISSION GATE (015 §Admission control) is one mechanism in four seal states:
//   Open       — admissions allowed (steady state).
//   Paused     — admissions deferred (drain budget 002 / overload 022); in-flight run to completion.
//   Draining   — admissions sealed; in-flight AWAITED to completion  (quiesce(Drain): snapshot 012).
//   Cancelling — admissions sealed; in-flight stop_tokens fired, then unwound (quiesce(Cancel):
//                Restart 007, forced shutdown).
// The quiescence primitive (`quiesce`) is the Draining/Cancelling seal plus a wait for the in-flight
// set to empty; bounded by a watchdog that escalates Drain→Cancel on deadline (015 §Bounded
// quiescence). 007 Restart uses quiesce(Cancel); 012 snapshot uses quiesce(Drain).
// ============================================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

#include "quark/core/config.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/dispatch.hpp"
#include "quark/core/error.hpp"
#include "quark/core/exec_state.hpp"
#include "quark/core/hot_cell.hpp"  // 013/022 Overflow + the LIVE bound/overflow/shed word (0-RMW read)
#include "quark/core/mailbox.hpp"
#include "quark/core/message_context.hpp"
#include "quark/core/metrics.hpp"  // 009: ShardCounters — engine-wired at registration (set_metrics)
#include "quark/core/task.hpp"  // async_frame_faulted (007 handler-boundary guard, async channel)
#include "pal/pal.hpp"          // pal::now() — the canonical suspend-counting clock (018/019)

namespace quark {

// ============================================================================================
// 007-Failure-and-Supervision / ADR-009 — the runtime supervision surface (the type-erased image
// of the compile-time `OnFailure<Decision, MaxRestarts<N, Within<…>>>` policy; the compile-time
// tags + the `supervision_of<A>()` extractor live in `supervision.hpp`, resolved by the engine at
// registration exactly like `max_concurrency_of<A>()`).
// ============================================================================================

// The four supervision directives (007 §Supervision decision). A handler FAILURE (throw / resource
// / poison-loop) runs the configured directive; a TRANSIENT failure (deadline / cancellation) is
// carved out and always Resumes without charging the restart budget (ADR-009 C5).
enum class SupervisionDirective : std::uint8_t {
    Resume = 0,    // keep actor state; drop the failed message; continue draining
    Restart = 1,   // reconstruct actor state (fresh); keep the mailbox; re-activate
    Stop = 2,      // deactivate the actor; drain the remaining mailbox to dead-letter
    Escalate = 3,  // hand the decision to the node supervisor (escalation seam)
};

// Failure-source classification (007 §Transient vs. actor failure). Deadline/Cancellation are
// TRANSIENT (Resume, no restart charge); the rest are ACTOR failures that run the directive.
enum class FailureSource : std::uint8_t {
    HandlerThrow = 0,  // a sync handler threw, or an async task<> completed with an exception (#1)
    Resource = 1,      // a PerMessage<T> factory failed (#4) — routed here as an actor failure
    PoisonLoop = 2,    // the same message repeatedly fails after restart (#5)
    Deadline = 3,      // the message deadline passed (#2) — TRANSIENT
    Cancellation = 4,  // the message stop_token fired (#3) — TRANSIENT
};

// Runtime supervision policy — the resolved `OnFailure<…>` for a type-erased Activation. Defaults
// match the spec: Restart, with an (effectively) unbounded budget until an explicit MaxRestarts.
struct SupervisionPolicy {
    SupervisionDirective decision = SupervisionDirective::Restart;
    std::uint32_t max_restarts = std::numeric_limits<std::uint32_t>::max();  // MaxRestarts<N>
    std::int64_t window_ns = 0;  // Within<…> as ns; 0 ⇒ no window (count never resets)
};

// Reconstruct seam (ADR-009 §Restart): reconstruct the actor instance's state IN PLACE (fresh state
// via the construction factory). The Activation is type-erased (holds `void* self`), so it cannot
// know how to rebuild the actor — the ENGINE (013) supplies this when it constructs the activation.
// Default (null) ⇒ Restart re-activates WITHOUT reconstructing (assert-intact); wire a factory to
// get true fresh-state Restart. REPORTED engine-cooperation seam (see report).
struct ReconstructSink {
    void (*fn)(void* self, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
    void operator()(void* self) const noexcept {
        if (fn) fn(self, ctx);
    }
};

// Dead-letter seam (007 §Per-message outcome; 022 §shed-don't-buffer). A FAILED `tell` (and the
// observability record of a failed `ask`) routes here with its error BEFORE the descriptor is
// reclaimed. Statically `noexcept`, bounded shed-don't-buffer at the sink (ADR-009 S4). Default
// (null) ⇒ counted-and-dropped; wire a sink to observe/replay.
struct DeadLetterSink {
    void (*fn)(Descriptor* d, error e, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
    void operator()(Descriptor* d, error e) const noexcept {
        if (fn) fn(d, e, ctx);
    }
};

// Escalation seam (007 §Escalation; ADR-009 `Supervision<Node|PerType|Tree<…>>`). `Escalate` (or a
// budget-exhausted `Restart`) TELLS the supervisor's lane — never a synchronous cross-lane touch,
// so single-executor is preserved. Full typed-tree topology is a 021/026 seam; the default node
// supervisor's action is `Stop`. Default (null) ⇒ escalation degrades to a local `Stop`.
struct EscalationSink {
    void (*fn)(void* self, error e, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
};

// Reclamation seam. 002/003 wire the shard-owned DescriptorPool here (a shard is drained by one
// worker at a time, so pool return is single-writer). Default = Descriptor::release() (the gen bump
// that fences a late cancel against reuse). A tombstone and a completed message both flow here.
struct ReclaimSink {
    void (*fn)(Descriptor*, void*) noexcept = nullptr;
    void* ctx = nullptr;
    QUARK_ALWAYS_INLINE void operator()(Descriptor* d) const noexcept {
        if (fn)
            fn(d, ctx);
        else
            d->release();
    }
};

// The 015 quiescence mode (015 §The quiescence primitive).
enum class QuiesceMode : std::uint8_t {
    Drain,   // let in-flight handlers finish (graceful: snapshot 012, deactivate 005, migration 010)
    Cancel,  // fire each in-flight stop_token, then await unwind (failure: Restart 007, shutdown)
};

// The 015 admission-gate seal (015 §Admission control — the shared mechanism).
enum class SealState : std::uint8_t {
    Open = 0,        // admissions allowed
    Paused = 1,      // admissions deferred (drain budget 002 / overload 022)
    Draining = 2,    // admissions sealed; in-flight awaited (quiesce(Drain))
    Cancelling = 3,  // admissions sealed; in-flight stop_tokens fired (quiesce(Cancel))
};

class Activation;

// The quiescence guard (015 step 4/5). While held, the caller has exclusive access to actor state
// with NO handler running. Destruction (step 5) clears the seal so the lane resumes admitting queued
// messages in FIFO order. Move-only; state-access only (no ActorRef-to-self — the guard-reentry
// hazard, 015 §Guard-reentry hazard). On a Sequential actor this is a no-op (act_ carries no seal).
class QuiescenceGuard {
public:
    QuiescenceGuard() noexcept = default;
    QuiescenceGuard(const QuiescenceGuard&) = delete;
    QuiescenceGuard& operator=(const QuiescenceGuard&) = delete;
    QuiescenceGuard(QuiescenceGuard&& o) noexcept : act_(std::exchange(o.act_, nullptr)) {}
    QuiescenceGuard& operator=(QuiescenceGuard&& o) noexcept {
        if (this != &o) {
            release();
            act_ = std::exchange(o.act_, nullptr);
        }
        return *this;
    }
    ~QuiescenceGuard() { release(); }

    [[nodiscard]] bool held() const noexcept { return act_ != nullptr; }
    void release() noexcept;  // defined after Activation (clears the seal)

private:
    friend class Activation;
    explicit QuiescenceGuard(Activation* a) noexcept : act_(a) {}
    Activation* act_ = nullptr;
};

class Activation {
public:
    enum class DrainOutcome : std::uint8_t {
        DrainedEmpty,     // head reached tail, no producer mid-publish, no in-flight ⇒ run close_out()
        BudgetExhausted,  // drain budget spent with (possibly) more work ⇒ yield_to_scheduled()
        Suspended,        // async handler(s) parked the activation (Running→Parked); 015 re-admits
        Busy,             // a producer is mid-publish (non-linearizable emptiness) ⇒ bounded spin
    };

    // `self` is the actor instance; `table` its ADR-007 dispatch table (by value); `reclaim` the
    // 002/003 pool seam. `max_concurrency` is the 005 declared intent resolved by the engine via
    // `max_concurrency_of<A>()` (policies.hpp): 1 = Sequential (default; no reentrant core), N>1 =
    // MaxConcurrency<N>, 0 = Reentrant (unbounded). The reentrant core is cold-allocated ONLY when
    // max_concurrency != 1, so Sequential actors instantiate nothing (015 §Zero cost).
    Activation(void* self, DispatchTable table, ReclaimSink reclaim = {},
               std::size_t max_concurrency = 1, SupervisionPolicy sup = {})
        : self_(self), table_(table), reclaim_(reclaim), sup_(sup) {
        // Activation-pooled stop_source (ADR-009): one control block per activation, allocated COLD
        // at construction — every Sequential message's MessageContext shares its token, so the hot
        // path allocates nothing. Reentrant frames carry per-frame stop_sources (see ReFrame) so
        // quiesce(Cancel) can fire each in-flight handler independently (015 step 2).
        current_ctx_.stop = stop_src_.get_token();
        if (max_concurrency != 1)
            rc_ = std::make_unique<ReCore>(max_concurrency);
    }

    // ---- Supervision wiring (007/ADR-009; cold, set at registration by the engine/harness) -----
    void set_supervision(SupervisionPolicy sup) noexcept { sup_ = sup; }
    void set_reconstruct(ReconstructSink r) noexcept { reconstruct_ = r; }
    void set_dead_letter_sink(DeadLetterSink s) noexcept { dead_letter_ = s; }

    // 009-Observability wiring (cold, set at registration by the engine): the SHARED per-shard
    // counter block this activation's shard owns. Null ⇒ unwired (standalone/test Activation usage
    // outside an Engine), every increment site below is guarded — zero behavior change when unset.
    void set_metrics(ShardCounters* sc) noexcept { metrics_ = sc; }

    // Injectable clock (014 §virtual clock). `fn(ctx)` returns "now" in ns on the SAME scale as
    // Descriptor::deadline_ns (pal::clock). The 014 SimEngine binds its virtual clock here so a
    // MaxRestarts<N,Within<W>> window + deadline shedding run deterministically under simulation
    // instead of reading the host wall clock (the leak the 014 audit proved). Default = real steady.
    using ClockFn = std::int64_t (*)(void* ctx) noexcept;
    void set_clock(ClockFn fn, void* ctx) noexcept { clock_fn_ = fn; clock_ctx_ = ctx; }
    void set_escalation_sink(EscalationSink s) noexcept { escalate_sink_ = s; }

    // ======================================================================================
    // 022-Resource-Governance-and-Overload-Control — the bounded mailbox + Overflow policy +
    // deadline-aware load shedding. PARTICIPATION is opt-in per activation (the spec's policy tag):
    // an ungoverned activation NEVER cold-allocates `gov_`, so `post()` and `drain_step()` stay the
    // verbatim ADR-002/004/015 hot path — zero cost, one predictable `if (gov_)` branch (mirrors the
    // accepted `if (rc_)` reentrant branch). The NUMBERS (bound, overflow, shed) are operational
    // config: when wired to the engine `HotCell` they are the LIVE, live-reconfigurable, 0-RMW word.
    // ======================================================================================

    // 022 admission verdict for the governed producer path. Admitted ⇒ the frame is in the mailbox
    // (`wake` valid). Blocked ⇒ Overflow::Block backpressure — NOT enqueued, NOT dead-lettered; the
    // caller backs off. Shed ⇒ Overflow::Fail/DropNewest refused the new frame — NOT enqueued; the
    // CALLER owns the descriptor (return it to the send pool / fail the `ask` cell). Neither Blocked
    // nor Shed touches the lane's single-writer pool/dead-letter sink, so single-executor holds.
    enum class AdmitResult : std::uint8_t { Admitted = 0, Blocked = 1, Shed = 2 };
    struct PostAdmission {
        AdmitResult result = AdmitResult::Admitted;
        bool wake = false;  // valid iff result == Admitted (the Idle→Scheduled wake edge, 002)
    };

    // Governance participation config (013/022). `hot` (the engine HotCell) supplies the LIVE
    // bound/overflow/shed word (0-RMW read, live-reconfigurable); pass nullptr to use the static
    // fallbacks. `deadline_shed` turns on 018 doomed-work shedding at admission; `shed_threshold`
    // gates it "under overload" (shed doomed work only once the resident depth reaches it; 0 ⇒
    // always shed a doomed message when `deadline_shed` is on).
    struct GovernanceConfig {
        const HotCell* hot = nullptr;
        std::uint32_t static_bound = 0;              // used iff hot == nullptr; 0 ⇒ unbounded
        Overflow static_overflow = Overflow::Block;  // used iff hot == nullptr
        bool deadline_shed = false;                  // 018 doomed-work shedding
        std::uint32_t shed_threshold = 0;            // resident depth that arms deadline shedding
    };

    // Enable governance (COLD; call once at registration). Cold-allocates the governance core.
    void enable_governance(GovernanceConfig gc) {
        gov_ = std::make_unique<GovernanceCore>(gc);
    }
    [[nodiscard]] bool is_governed() const noexcept { return gov_ != nullptr; }

    // The governed producer admission (022 §Load shedding — shed at admission). Enforces the bound +
    // Overflow policy BEFORE the verbatim mailbox enqueue. Many producers may call this concurrently
    // (the depth counter is a producer-side atomic — the enqueue path already carries a `tail_`
    // exchange RMW; this adds one more producer-side RMW, never a consumer-drain RMW).
    [[nodiscard]] PostAdmission post_governed(Descriptor* d) noexcept {
        GovernanceCore& g = *gov_;
        const std::uint32_t bound = g.bound();
        if (bound != 0) {
            const std::uint64_t depth = g.depth();
            if (depth >= bound) {
                switch (g.overflow()) {
                    case Overflow::Block:
                        // Producer-side counter: many producers ⇒ fetch_add (a relaxed load+store
                        // would lose concurrent increments). This is NOT the consumer drain path.
                        g.blocks.fetch_add(1, std::memory_order_relaxed);
                        return {AdmitResult::Blocked, false};
                    case Overflow::Fail:
                    case Overflow::DropNewest:
                        g.sheds.fetch_add(1, std::memory_order_relaxed);
                        return {AdmitResult::Shed, false};
                    case Overflow::DropOldest:
                        break;  // admit the newest; the lane sheds the oldest down to the bound
                }
            }
        }
        g.enqueued.fetch_add(1, std::memory_order_relaxed);  // producer-side depth++
        mailbox_.enqueue(d);
        Mailbox::producer_close_out_fence();
        if (metrics_) metrics_->mailbox_enqueued.inc_atomic();  // producer-side, possibly concurrent
        const bool wake = exec_.notify_enqueued();
        if (metrics_ && wake) metrics_->activations.inc_atomic();  // Idle->Scheduled edge (009)
        return {AdmitResult::Admitted, wake};
    }

    // ---- Governance observers (009 — every shed/block is counted) --------------------------
    [[nodiscard]] std::uint64_t mailbox_depth() const noexcept { return gov_ ? gov_->depth() : 0; }
    [[nodiscard]] std::uint64_t governance_sheds() const noexcept {
        return gov_ ? gov_->sheds.load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] std::uint64_t governance_blocks() const noexcept {
        return gov_ ? gov_->blocks.load(std::memory_order_relaxed) : 0;
    }

    Activation(const Activation&) = delete;
    Activation& operator=(const Activation&) = delete;
    Activation(Activation&&) = delete;  // holds atomics + a Mailbox (non-movable)
    Activation& operator=(Activation&&) = delete;

    // ---- Producer / wake seam (002) ------------------------------------------------------
    QUARK_ALWAYS_INLINE bool post(Descriptor* d) noexcept {
        mailbox_.enqueue(d);
        Mailbox::producer_close_out_fence();  // elided on x86-TSO (the tail_.exchange fenced it)
        if (metrics_) metrics_->mailbox_enqueued.inc_atomic();  // producer-side, possibly concurrent
        const bool wake = exec_.notify_enqueued();
        if (metrics_ && wake) metrics_->activations.inc_atomic();  // Idle->Scheduled edge (009)
        return wake;
    }
    QUARK_ALWAYS_INLINE bool notify_enqueued() noexcept {
        Mailbox::producer_close_out_fence();
        const bool wake = exec_.notify_enqueued();
        if (metrics_ && wake) metrics_->activations.inc_atomic();  // Idle->Scheduled edge (009)
        return wake;
    }

    // ---- Worker ownership seam (002) -----------------------------------------------------
    [[nodiscard]] QUARK_ALWAYS_INLINE bool try_acquire() noexcept { return exec_.try_acquire(); }

    // ---- The drain (single-executor; call ONLY while holding Running) ---------------------
    DrainOutcome drain_step(std::uint32_t budget) noexcept {
        if (rc_) return drain_step_reentrant(budget);  // 015 reentrant path (one predictable branch)
        if (QUARK_UNLIKELY(gov_)) return drain_step_governed_seq(budget);  // 022 governed sequential

        // ====================================================================================
        // Sequential path — VERBATIM 001 single-in-flight park. Zero 015 machinery is touched.
        // ====================================================================================
        std::uint32_t remaining = budget;
        for (;;) {
            if (remaining == 0) return DrainOutcome::BudgetExhausted;

            const DrainResult r = mailbox_.try_dequeue();
            if (r.status == DrainStatus::Empty) return DrainOutcome::DrainedEmpty;
            if (r.status == DrainStatus::Busy) return DrainOutcome::Busy;

            --remaining;
            Descriptor* d = r.desc;

            if (!d->try_claim()) {
                reclaim_(d);
                continue;
            }

            // 007 §Stop: a Stopped actor dispatches nothing — its survivors drain to dead-letter.
            if (QUARK_UNLIKELY(stopped_)) {
                dead_letter_and_reclaim(d, error{errc::supervised_stop, "actor_stopped"});
                continue;
            }

            current_ctx_.deadline_ns = d->deadline_ns;
            current_ctx_.trace_id = d->trace_id;

            // ==== ADR-009 D1 handler-boundary guard (zero-cost on the no-throw path) ============
            // The ONE try/catch that contains a throwing handler. Itanium ABI: the success path pays
            // NOTHING (no added instruction on the hot BB — the landing pad lives in a cold section,
            // proven by objdump + bench, ADR-009 F1/F2). A SYNC throw (and a coroutine-frame alloc
            // failure starting an async handler) unwinds cleanly to here; the fault is turned into a
            // VALUE (failed reply / dead-letter) and the supervision decision is applied.
            DispatchOutcome o;
#ifndef QUARK_SUPERVISION_NO_GUARD
            try {
                detail::AmbientContextScope amb(current_ctx_);  // #12: tell/ask from handler inherits
                o = table_.thunks[slot_from(*d)](self_, d, current_ctx_);
            } catch (...) {
                on_fault_sequential(d, FailureSource::HandlerThrow);
                continue;  // Resume/Restart drain on; Stop/Escalate flip stopped_ (checked above)
            }
#else
            // CONTROL (zero-cost proof): no guard. A throw escapes into `noexcept` ⇒ terminate; the
            // control benchmark uses only non-throwing handlers, isolating the guard's success cost.
            {
                detail::AmbientContextScope amb(current_ctx_);
                o = table_.thunks[slot_from(*d)](self_, d, current_ctx_);
            }
#endif

            if (o.kind == HandlerKind::Sync) {
                d->complete();
                reclaim_(d);
                if (metrics_) metrics_->messages_processed.inc();  // drain-owner-exclusive
                continue;
            }

            if (o.frame.done()) {
                // Async handler completed inline. It may have completed WITH an exception (surfaced
                // at final_suspend, 007) — probe the promise and route it through the guard.
                if (QUARK_UNLIKELY(async_frame_faulted(o.frame))) {
                    o.frame.destroy();
                    on_fault_sequential(d, FailureSource::HandlerThrow);
                    continue;
                }
                o.frame.destroy();
                d->complete();
                reclaim_(d);
                if (metrics_) metrics_->messages_processed.inc();  // drain-owner-exclusive
                continue;
            }

            parked_frame_ = o.frame;
            parked_desc_ = d;
            exec_.park();  // Running→Parked (release); seals every admission CAS (ADR-015)
            return DrainOutcome::Suspended;
        }
    }

    // ---- Close-out seam (002): DrainedEmpty → relinquish via the seq_cst Dekker rendezvous ----
    [[nodiscard]] bool close_out() noexcept {
        exec_.release_to_idle();
        Mailbox::consumer_close_out_fence();
        if (!mailbox_.probe_has_work()) return false;
        return exec_.reacquire_from_idle();
    }

    // ---- Fairness seam (002): BudgetExhausted / Busy → Running→Scheduled -------------------
    QUARK_ALWAYS_INLINE void yield_to_scheduled() noexcept { exec_.yield_to_scheduled(); }

    // ---- 001/Sequential completion seam (async / blocking / fiber) ------------------------
    // Minimal single-frame completion the engine (002) drives on the Parked→Scheduled edge. Kept
    // BACKWARD-COMPATIBLE for the Sequential path (engine.hpp, exec_suspend/sched_readmit tests).
    bool complete_parked() noexcept {
        Descriptor* d = parked_desc_;
        // Guard the resume: an async handler surfaces its throw at completion (007). A body throw is
        // captured by the promise (probe via async_frame_faulted); a stray propagation is caught too.
        bool faulted = false;
#ifndef QUARK_SUPERVISION_NO_GUARD
        try {
            detail::AmbientContextScope amb(current_ctx_);  // #12: a tell after co_await still inherits
            parked_frame_.resume();
        } catch (...) {
            faulted = true;
        }
        if (!faulted && async_frame_faulted(parked_frame_)) faulted = true;
#else
        {
            detail::AmbientContextScope amb(current_ctx_);
            parked_frame_.resume();
        }
#endif
        parked_frame_.destroy();
        parked_frame_ = {};
        parked_desc_ = nullptr;
        if (QUARK_UNLIKELY(faulted)) {
            on_fault_sequential(d, FailureSource::HandlerThrow);  // record outcome + supervise
            return exec_.readmit_from_parked();
        }
        d->complete();
        reclaim_(d);
        if (metrics_) metrics_->messages_processed.inc();  // drain-owner-exclusive (engine-driven)
        return exec_.readmit_from_parked();
    }
    [[nodiscard]] bool is_parked() const noexcept { return parked_desc_ != nullptr; }

    // ======================================================================================
    // 015 REENTRANT surface (no-op / absent on Sequential actors).
    // ======================================================================================

    // Handler-facing suspension point (reentrant). A `task<>` handler `co_await`s this to yield its
    // frame to the activation's in-flight set; a carrier later completes it via `complete_one()`.
    // Registers the CURRENTLY-STARTING frame (lane-private `resuming_`) as suspended, so a carrier
    // on another thread can re-admit it through the gate. (Any awaitable works — a handler that
    // awaits e.g. `std::suspend_always{}` is caught by the admit/resume fallback registration.)
    struct AsyncSuspend {
        Activation* act;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept { act->on_async_suspend(); }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] AsyncSuspend async_suspend() noexcept { return AsyncSuspend{this}; }

    // Carrier seam (any thread): complete ONE in-flight async op — re-admit the oldest suspended
    // frame through the gate (ready-queue push + Parked→Scheduled). Returns should_wake. This is the
    // real 015 admission edge that replaces 001's minimal `complete_parked` on the reentrant path:
    // the frame is RESUMED on the actor's lane (never inline on the carrier), preserving
    // single-executor across async completion. No-op (returns false) on a Sequential actor.
    bool complete_one() noexcept {
        if (!rc_) return false;
        {
            std::lock_guard<std::mutex> lk(rc_->mtx);
            if (rc_->suspended.empty()) return false;
            ReFrame* f = rc_->suspended.front();
            rc_->suspended.pop_front();
            rc_->ready.push_back(f);
        }
        Mailbox::producer_close_out_fence();     // pair the lane's Parked close-out (Dekker)
        return exec_.readmit_from_parked();      // Parked→Scheduled; true ⇒ wake exactly one worker
    }

    // The 015 quiescence primitive. `co_await act.quiesce(mode)` from a handler: seal admission,
    // (Cancel) fire in-flight stop_tokens, await the in-flight set to drain, then resolve to a
    // QuiescenceGuard (015 §The quiescence primitive). `drain_deadline_ns` (>0, Drain only) arms the
    // bounded-quiescence watchdog (015 §Bounded quiescence) — on expiry `poll_quiesce_watchdog`
    // escalates Drain→Cancel. On a Sequential actor the actor is always at a quiescent point between
    // messages, so this resolves SYNCHRONOUSLY and the guard is a no-op.
    struct QuiesceAwaiter {
        Activation* act;
        QuiesceMode mode;
        [[nodiscard]] bool await_ready() noexcept { return act->begin_quiesce(mode); }
        void await_suspend(std::coroutine_handle<> h) noexcept { act->suspend_quiesce_waiter(h); }
        [[nodiscard]] QuiescenceGuard await_resume() noexcept { return QuiescenceGuard{act}; }
    };
    [[nodiscard]] QuiesceAwaiter quiesce(QuiesceMode mode, std::int64_t drain_deadline_ns = 0) noexcept {
        if (rc_ && mode == QuiesceMode::Drain && drain_deadline_ns != 0)
            rc_->drain_deadline_ns.store(drain_deadline_ns, std::memory_order_release);  // 0 = disarmed
        return QuiesceAwaiter{this, mode};
    }

    // Bounded-quiescence watchdog (015 §Bounded quiescence). Drive from the lane thread (or the 011
    // timer tick on the lane). If a Drain has exceeded its deadline, escalate to Cancel — fire the
    // in-flight stop_tokens and drive their resume so a stuck/slow handler cannot stall Drain
    // forever. Returns true iff it escalated. A second-deadline escalation to the node supervisor
    // (007) is a documented seam. No-op on Sequential / when not Draining / when disarmed.
    // OFF-LANE SAFE (audit Finding 1): reads only atomics; it does NOT touch `seal`/`live` directly.
    // On expiry it raises the `kEscalateCancel` signal and wakes the lane; the lane performs the
    // Draining→Cancelling seal + fire_cancel_all ON-LANE (apply_lane_actions). Returns true iff it
    // woke a Parked lane (a Running/Scheduled lane self-observes the signal on its next loop).
    bool poll_quiesce_watchdog(std::int64_t now_ns) noexcept {
        if (!rc_) return false;
        if (rc_->seal.load(std::memory_order_acquire) != SealState::Draining) return false;
        const std::int64_t dl = rc_->drain_deadline_ns.load(std::memory_order_acquire);
        if (dl == 0 || now_ns < dl) return false;
        rc_->lane_actions.fetch_or(ReCore::kEscalateCancel, std::memory_order_acq_rel);
        Mailbox::producer_close_out_fence();     // pair the lane's Parked close-out (Dekker)
        return exec_.readmit_from_parked();      // Parked→Scheduled; wakes a stuck-Parked lane
    }

    // Admin admission pause (drain budget 002 / overload 022) — the SAME gate, Paused seal. New
    // admissions defer while in-flight run to completion; `resume_admission` re-opens the gate.
    // OFF-LANE SAFE: an atomic CAS on `seal`. A lane transition (Open→Draining) racing an off-lane
    // pause is resolved by the CAS — whoever wins, admission ends up sealed, which is correct.
    void pause_admission() noexcept {
        if (!rc_) return;
        SealState e = SealState::Open;
        rc_->seal.compare_exchange_strong(e, SealState::Paused, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
    }
    void resume_admission() noexcept {
        if (!rc_) return;
        SealState e = SealState::Paused;
        rc_->seal.compare_exchange_strong(e, SealState::Open, std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
    }

    // ---- Cancellation seam (001) — OFF-LANE SAFE -----------------------------------------
    // The activation-level stop_source is thread-safe. The PER-FRAME in-flight stop_tokens must be
    // fired while walking `live`, which is lane-only — so raise `kStopAll` and wake; the lane fires
    // them ON-LANE (apply_lane_actions). Cooperative cancellation tolerates the tiny hand-off delay.
    void request_stop() noexcept {
        stop_src_.request_stop();
        if (rc_) {
            rc_->lane_actions.fetch_or(ReCore::kStopAll, std::memory_order_acq_rel);
            Mailbox::producer_close_out_fence();
            (void)exec_.readmit_from_parked();  // best-effort wake if Parked; else lane self-observes
        }
    }

    // ---- Observers ------------------------------------------------------------------------
    [[nodiscard]] ExecState state() const noexcept { return exec_.state(); }
    [[nodiscard]] Mailbox& mailbox() noexcept { return mailbox_; }
    [[nodiscard]] const MessageContext& current_context() const noexcept { return current_ctx_; }
    [[nodiscard]] bool is_reentrant() const noexcept { return rc_ != nullptr; }
    // In-flight set size = handlers started but not completed (015 §Definitions). 0 for Sequential
    // between messages (always a quiescent point).
    [[nodiscard]] std::size_t in_flight() const noexcept {
        return rc_ ? rc_->live_count.load(std::memory_order_acquire) : 0;
    }
    // High-water mark of concurrent in-flight handlers — the MaxConcurrency<N> observation.
    [[nodiscard]] std::size_t max_in_flight() const noexcept {
        return rc_ ? rc_->observed_max.load(std::memory_order_acquire) : 0;
    }
    [[nodiscard]] std::size_t suspended_count() const noexcept {
        if (!rc_) return 0;
        std::lock_guard<std::mutex> lk(rc_->mtx);
        return rc_->suspended.size();
    }
    [[nodiscard]] SealState seal() const noexcept {
        return rc_ ? rc_->seal.load(std::memory_order_acquire) : SealState::Open;
    }

    // ---- 007/ADR-009 supervision observers (test/observability seams) ---------------------
    [[nodiscard]] bool is_stopped() const noexcept { return stopped_; }
    [[nodiscard]] std::uint32_t restarts_total() const noexcept { return restarts_total_; }
    [[nodiscard]] std::uint32_t restart_window_count() const noexcept { return restart_count_; }
    [[nodiscard]] std::uint32_t escalations() const noexcept { return escalations_; }
    [[nodiscard]] std::uint32_t faults() const noexcept { return faults_; }
    [[nodiscard]] std::uint32_t dead_letters() const noexcept { return dead_letters_; }
    [[nodiscard]] const SupervisionPolicy& supervision() const noexcept { return sup_; }

private:
    friend class QuiescenceGuard;

    // ---- Reentrant frame: one live handler (started, not yet completed) --------------------
    // Heap-allocated (unique_ptr) so its `ctx` address is STABLE for the frame's whole life — the
    // handler holds `const MessageContext&` across suspensions. Carries a PER-FRAME stop_source so
    // quiesce(Cancel) can fire each in-flight handler independently (015 step 2 / ADR-009).
    struct ReFrame {
        std::coroutine_handle<> h{};
        Descriptor* desc = nullptr;
        MessageContext ctx{};
        std::stop_source stop{};
    };

    // ---- Reentrant core: cold-allocated ONLY for Reentrant / MaxConcurrency<N> -------------
    struct ReCore {
        explicit ReCore(std::size_t max_concurrency) noexcept
            : cap(max_concurrency == 0 ? std::numeric_limits<std::size_t>::max() : max_concurrency) {}
        std::size_t cap;  // admission ceiling (SIZE_MAX == unbounded Reentrant); the frame-pool bound

        // ---- lane-private CONTAINERS (mutated ONLY by the worker holding Running) ----------
        std::vector<std::unique_ptr<ReFrame>> live;  // frame pool = in-flight set (LANE-ONLY)
        ReFrame* resuming_frame = nullptr;           // frame being started/resumed right now
        bool resuming_suspended = false;             // set by on_async_suspend for resuming_frame
        ReFrame* waiter_frame = nullptr;             // the suspended quiesce() continuation (0 or 1)
        bool restarting = false;  // 007/ADR-009 S3 restart-episode marker: a Restart is unwinding
                                  // its in-flight siblings (seal→quiescence window). A sibling that
                                  // faults WHILE this is set is absorbed (dead-lettered) — it does
                                  // NOT re-charge MaxRestarts or nest a second restart.

        // ---- cross-thread-visible SCALARS. Off-lane callers — 022 pause/resume, the 011 bounded-
        //      quiescence watchdog driver, 009 observers — read/CAS these ATOMICALLY. Every state
        //      mutation that must WALK `live` still runs on the lane (see `lane_actions`), so the
        //      containers above stay lane-only; only these scalars cross threads (audit Finding 1).
        std::atomic<SealState> seal{SealState::Open};
        std::atomic<std::size_t> live_count{0};         // == live.size(), published for observers (009)
        std::atomic<std::size_t> observed_max{0};       // MaxConcurrency high-water (009)
        std::atomic<std::int64_t> drain_deadline_ns{0}; // 0 == disarmed; armed on-lane in quiesce()
        // Off-lane → lane signal bitset: an off-lane caller sets a bit + wakes the lane; the lane
        // drains it at the top of drain_step_reentrant and runs the action ON-LANE (no off-lane
        // `live`/`seal` mutation — the whole point of Finding 1's fix).
        static constexpr std::uint32_t kEscalateCancel = 1u;  // Drain→Cancel (bounded quiescence 015)
        static constexpr std::uint32_t kStopAll = 2u;         // fire every in-flight stop_token (007)
        std::atomic<std::uint32_t> lane_actions{0};

        // ---- carrier <-> lane hand-off (guarded; frames + this queue pair cross threads) ---
        mutable std::mutex mtx;
        std::deque<ReFrame*> suspended;  // parked-on-carrier, available for complete_one()
        std::deque<ReFrame*> ready;      // carrier-completed / cancel-driven, to resume on the lane
    };

    // ---- 022 governance core: cold-allocated ONLY for a governed activation --------------------
    // Bounded-mailbox depth accounting is a producer/consumer counter PAIR (never one shared RMW):
    //   * `enqueued` — many producers `fetch_add` (producer-side RMW, alongside the mailbox
    //     `tail_.exchange` the enqueue already does — never a drain-side RMW);
    //   * `drained`  — the SINGLE draining worker publishes via a relaxed load+store (the 009 metrics
    //     Counter idiom: a `mov`/`mov` pair, NOT a `lock xadd`), so the consumer drain adds 0
    //     cross-core RMW (CONVENTIONS.md / 023) while staying TSan-clean (relaxed atomic, not a
    //     plain int). depth = enqueued − drained (a producer-visible estimate; approximate is the
    //     right trade for overload protection — spec §Self-debate).
    struct GovernanceCore {
        explicit GovernanceCore(GovernanceConfig gc) noexcept : cfg(gc) {}
        GovernanceConfig cfg;

        std::atomic<std::uint64_t> enqueued{0};  // producer fetch_add
        std::atomic<std::uint64_t> drained{0};   // consumer single-writer relaxed (load+store)
        std::atomic<std::uint64_t> sheds{0};      // dropped frames (DropNewest/Fail/DropOldest/deadline)
        std::atomic<std::uint64_t> blocks{0};     // Overflow::Block backpressure events

        // The LIVE bound/overflow read the 0-RMW HotCell word when wired; else the static fallback.
        [[nodiscard]] std::uint32_t bound() const noexcept {
            return cfg.hot ? cfg.hot->mailbox_bound() : cfg.static_bound;
        }
        [[nodiscard]] Overflow overflow() const noexcept {
            return cfg.hot ? cfg.hot->overflow() : cfg.static_overflow;
        }
        [[nodiscard]] std::uint64_t depth() const noexcept {
            return enqueued.load(std::memory_order_relaxed) - drained.load(std::memory_order_relaxed);
        }
        QUARK_ALWAYS_INLINE void note_shed() noexcept {
            sheds.store(sheds.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    };
    std::unique_ptr<GovernanceCore> gov_{};  // null for an ungoverned activation (zero cost)

    // ====================================================================================
    // 022 governed SEQUENTIAL drain. Mirrors the verbatim 001 sequential drain (single in-flight,
    // supervision guard, park) with three admission-side governance hooks that run BEFORE a message
    // consumes a cycle — all on the lane (single-writer), so shedding never adds a second executor:
    //   1. publish the consumer drain count (relaxed store — 0 cross-core RMW);
    //   2. Overflow::DropOldest — while the resident set is over the bound, dead-letter the OLDEST
    //      (`errc::overloaded`), preserving newest-wins FIFO for the admitted survivors;
    //   3. deadline-aware shedding (018) — a doomed message (deadline already passed) is shed FIRST
    //      and cheapest with `errc::timeout`, before it consumes a cycle (spec §Load shedding).
    // A message that survives the hooks dispatches identically to the sequential path.
    // ====================================================================================
    DrainOutcome drain_step_governed_seq(std::uint32_t budget) noexcept {
        GovernanceCore& g = *gov_;
        const std::uint32_t bound = g.bound();
        const bool drop_oldest = bound != 0 && g.overflow() == Overflow::DropOldest;
        const bool shed_doomed = g.cfg.deadline_shed;
        std::uint32_t remaining = budget;
        for (;;) {
            if (remaining == 0) return DrainOutcome::BudgetExhausted;

            const DrainResult r = mailbox_.try_dequeue();
            if (r.status == DrainStatus::Empty) return DrainOutcome::DrainedEmpty;
            if (r.status == DrainStatus::Busy) return DrainOutcome::Busy;

            --remaining;
            Descriptor* d = r.desc;

            // depth INCLUDING d (drained not yet bumped), then publish that d left the queue.
            const std::uint64_t resident = g.enqueued.load(std::memory_order_relaxed) -
                                           g.drained.load(std::memory_order_relaxed);
            g.drained.store(g.drained.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

            if (!d->try_claim()) {
                reclaim_(d);  // late-cancel tombstone
                continue;
            }
            if (QUARK_UNLIKELY(stopped_)) {
                dead_letter_and_reclaim(d, error{errc::supervised_stop, "actor_stopped"});
                continue;
            }

            // (2) DropOldest: d is the current oldest; shed it while the resident set exceeds bound.
            if (drop_oldest && resident > bound) {
                g.note_shed();
                dead_letter_and_reclaim(d, error{errc::overloaded, "shed_oldest"});
                continue;
            }
            // (3) deadline-aware shedding (018): doomed work, gated "under overload" by shed_threshold.
            // now_ns() reads the steady clock (== pal::clock, the basis of Descriptor::deadline_ns).
            if (shed_doomed && d->deadline_ns != 0 &&
                resident >= g.cfg.shed_threshold &&
                d->deadline_ns <= now_ns()) {
                g.note_shed();
                dead_letter_and_reclaim(d, error{errc::timeout, "deadline_shed"});
                continue;
            }

            current_ctx_.deadline_ns = d->deadline_ns;
            current_ctx_.trace_id = d->trace_id;

            DispatchOutcome o;
#ifndef QUARK_SUPERVISION_NO_GUARD
            try {
                detail::AmbientContextScope amb(current_ctx_);  // #12: tell/ask from handler inherits
                o = table_.thunks[slot_from(*d)](self_, d, current_ctx_);
            } catch (...) {
                on_fault_sequential(d, FailureSource::HandlerThrow);
                continue;
            }
#else
            {
                detail::AmbientContextScope amb(current_ctx_);
                o = table_.thunks[slot_from(*d)](self_, d, current_ctx_);
            }
#endif

            if (o.kind == HandlerKind::Sync) {
                d->complete();
                reclaim_(d);
                if (metrics_) metrics_->messages_processed.inc();  // drain-owner-exclusive
                continue;
            }
            if (o.frame.done()) {
                if (QUARK_UNLIKELY(async_frame_faulted(o.frame))) {
                    o.frame.destroy();
                    on_fault_sequential(d, FailureSource::HandlerThrow);
                    continue;
                }
                o.frame.destroy();
                d->complete();
                reclaim_(d);
                if (metrics_) metrics_->messages_processed.inc();  // drain-owner-exclusive
                continue;
            }
            parked_frame_ = o.frame;
            parked_desc_ = d;
            exec_.park();
            return DrainOutcome::Suspended;
        }
    }

    // ====================================================================================
    // 015 reentrant drain. Resumes carrier-completed frames on THIS lane (their synchronous
    // regions serialize here — no state races), then admits new messages up to the cap while the
    // gate is Open, then either closes out (no in-flight) or parks for carrier re-admission.
    // ====================================================================================
    DrainOutcome drain_step_reentrant(std::uint32_t budget) noexcept {
        ReCore& c = *rc_;
        std::uint32_t remaining = budget;
        for (;;) {
            // Drain off-lane signals (watchdog escalation / forced-stop) and run them ON-LANE, so all
            // `seal`/`live` mutation stays lane-private (audit Finding 1). Cheap: one relaxed exchange.
            if (const std::uint32_t acts = c.lane_actions.exchange(0, std::memory_order_acq_rel))
                apply_lane_actions(acts);

            drain_ready();  // resume carrier-completed frames (+ the quiesce waiter) on the lane

            if (c.seal.load(std::memory_order_acquire) == SealState::Open && c.live.size() < c.cap) {
                if (remaining == 0) return DrainOutcome::BudgetExhausted;
                const DrainResult r = mailbox_.try_dequeue();
                if (r.status == DrainStatus::Busy) return DrainOutcome::Busy;
                if (r.status == DrainStatus::Message) {
                    --remaining;
                    admit(r.desc);   // claim + dispatch; may complete inline or join the in-flight set
                    continue;
                }
                // Empty ⇒ fall through to the relinquish decision.
            }

            if (has_pending_ready()) continue;  // a carrier completed something ⇒ loop to resume it

            if (c.live.empty() && c.seal.load(std::memory_order_acquire) == SealState::Open)
                return DrainOutcome::DrainedEmpty;  // quiescent + no work ⇒ close_out to Idle

            // In-flight frames remain (or the gate is sealed / at cap). Park and let carriers
            // re-admit. Dekker close-out for the ready hand-off (mirrors the mailbox close-out):
            // store Parked, seq_cst fence, then re-probe — so a carrier push that races the park is
            // never lost.
            exec_.park();
            Mailbox::consumer_close_out_fence();
            const bool admittable = c.seal.load(std::memory_order_acquire) == SealState::Open &&
                                    c.live.size() < c.cap && mailbox_.probe_has_work();
            if (has_pending_ready() || admittable) {
                if (exec_.reacquire_from_parked()) continue;  // won the race ⇒ keep draining
                // else a carrier already re-admitted (Parked→Scheduled) and will wake a worker.
            }
            return DrainOutcome::Suspended;
        }
    }

    // Claim + dispatch one message on the reentrant lane. Sync / inline-complete handlers finish
    // here (never counted as concurrent in-flight); a truly-suspended async handler joins the
    // in-flight set. The frame is pushed to `live` BEFORE it is started so a carrier that completes
    // it the instant it suspends always finds it registered (no admit/complete race window).
    void admit(Descriptor* d) noexcept {
        if (!d->try_claim()) {
            reclaim_(d);  // late-cancel tombstone: one free, NO handler runs (001 §Cancellation)
            return;
        }
        if (QUARK_UNLIKELY(stopped_)) {  // 007 §Stop: Stopped ⇒ dead-letter, no dispatch
            dead_letter_and_reclaim(d, error{errc::supervised_stop, "actor_stopped"});
            return;
        }
        auto frame = std::make_unique<ReFrame>();
        ReFrame* f = frame.get();
        f->desc = d;
        f->ctx.deadline_ns = d->deadline_ns;
        f->ctx.trace_id = d->trace_id;
        f->stop = std::stop_source{};
        f->ctx.stop = f->stop.get_token();
        rc_->live.push_back(std::move(frame));
        rc_->live_count.store(rc_->live.size(), std::memory_order_release);  // publish for observers

        rc_->resuming_frame = f;
        rc_->resuming_suspended = false;
        // Handler-boundary guard (reentrant): a sync throw / frame-alloc failure unwinds to here.
        DispatchOutcome o;
#ifndef QUARK_SUPERVISION_NO_GUARD
        try {
            detail::AmbientContextScope amb(f->ctx);  // #12: tell/ask from this frame inherits its ctx
            o = table_.thunks[slot_from(*d)](self_, d, f->ctx);
        } catch (...) {
            rc_->resuming_frame = nullptr;
            on_fault_reentrant(f, FailureSource::HandlerThrow);
            return;
        }
#else
        {
            detail::AmbientContextScope amb(f->ctx);
            o = table_.thunks[slot_from(*d)](self_, d, f->ctx);
        }
#endif
        rc_->resuming_frame = nullptr;

        if (o.kind == HandlerKind::Sync) {
            complete_and_remove(f);  // sync ran to completion inline on the lane
            return;
        }
        f->h = o.frame;
        if (o.frame.done()) {
            if (QUARK_UNLIKELY(async_frame_faulted(o.frame))) {  // async completed with a throw (007)
                on_fault_reentrant(f, FailureSource::HandlerThrow);
                return;
            }
            complete_and_remove(f);  // async body ran to completion with no real suspension
            return;
        }
        // Real suspension: this frame is now concurrently in-flight. Record the high-water mark.
        if (rc_->live.size() > rc_->observed_max.load(std::memory_order_relaxed))
            rc_->observed_max.store(rc_->live.size(), std::memory_order_release);
        if (!rc_->resuming_suspended)
            register_suspended(f);  // fallback: handler awaited a non-registering awaitable
    }

    // Resume every carrier-completed / cancel-driven frame on THIS lane, one at a time (their
    // synchronous regions never overlap ⇒ no state races). A frame that completes is reclaimed and
    // removed from the in-flight set; a frame that suspends again re-registers.
    void drain_ready() noexcept {
        for (;;) {
            ReFrame* f = pop_ready();
            if (f == nullptr) return;
            rc_->resuming_frame = f;
            rc_->resuming_suspended = false;
            // Guard the resume. A cancelled sibling that observes its stop_token and unwinds via a
            // THROW (rather than co_return) surfaces here; its body throw is captured by the promise.
#ifndef QUARK_SUPERVISION_NO_GUARD
            bool threw = false;
            try {
                detail::AmbientContextScope amb(f->ctx);  // #12: a tell after co_await inherits f->ctx
                f->h.resume();
            } catch (...) {
                threw = true;
            }
            rc_->resuming_frame = nullptr;
            if (threw || (f->h.done() && async_frame_faulted(f->h))) {
                on_fault_reentrant(f, FailureSource::HandlerThrow);
                continue;
            }
#else
            {
                detail::AmbientContextScope amb(f->ctx);
                f->h.resume();
            }
            rc_->resuming_frame = nullptr;
#endif
            if (f->h.done())
                complete_and_remove(f);
            else if (!rc_->resuming_suspended)
                register_suspended(f);  // re-suspended via a non-registering awaitable
        }
    }

    // Finish a frame: destroy the coroutine, complete + reclaim its descriptor (the single
    // reclamation join point — the reply cell 006 resolves inside the handler body BEFORE this),
    // drop it from the in-flight set, then — if a quiesce waiter is parked and only it remains —
    // re-admit the waiter so it resolves to its guard AFTER every sibling has left (015 step 3;
    // ADR-009 S2: every sibling reply cell completes before state teardown).
    void complete_and_remove(ReFrame* f) noexcept {
        if (f->h) f->h.destroy();  // null for a sync handler (no coroutine frame); valid for async
        f->desc->complete();
        reclaim_(f->desc);
        if (metrics_) metrics_->messages_processed.inc();  // lane-only: the reentrant success join point
        remove_live(f);
        maybe_resume_waiter();
        finish_restart_if_drained();  // 007: the last cancelled sibling draining finishes a Restart
    }

    void maybe_resume_waiter() noexcept {
        ReCore& c = *rc_;
        if (c.seal.load(std::memory_order_acquire) == SealState::Open || c.waiter_frame == nullptr)
            return;
        // The waiter is itself a live frame; when it is the ONLY one left, all siblings have drained.
        if (c.live.size() == 1 && c.live.front().get() == c.waiter_frame) {
            ReFrame* w = c.waiter_frame;
            c.waiter_frame = nullptr;
            std::lock_guard<std::mutex> lk(c.mtx);
            c.ready.push_back(w);  // resume the waiter on the lane ⇒ QuiescenceGuard
        }
    }

    void remove_live(ReFrame* f) noexcept {
        auto& v = rc_->live;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].get() == f) {
                v[i] = std::move(v.back());
                v.pop_back();
                rc_->live_count.store(v.size(), std::memory_order_release);  // publish for observers
                return;
            }
        }
    }

    // ---- quiesce internals (all run on the lane, inside the quiescing handler) -------------
    // Seal the gate; (Cancel) fire in-flight stop_tokens + drive their resume. Return true iff the
    // actor is ALREADY quiescent (no OTHER in-flight handler) ⇒ the awaiter resolves synchronously.
    bool begin_quiesce(QuiesceMode mode) noexcept {
        if (!rc_) return true;  // Sequential: always a quiescent point between/within its one handler
        ReCore& c = *rc_;
        c.seal.store((mode == QuiesceMode::Cancel) ? SealState::Cancelling : SealState::Draining,
                     std::memory_order_release);
        if (mode == QuiesceMode::Cancel) fire_cancel_all();
        // The calling frame is itself in `live`; "others" = live.size() - 1.
        return c.live.size() <= 1;
    }

    // Suspend the quiescing handler as the single waiter (015: one small waiter frame). NOT added to
    // `suspended` (no carrier may complete it) — it resumes only when the in-flight set drains.
    void suspend_quiesce_waiter(std::coroutine_handle<>) noexcept {
        rc_->resuming_suspended = true;      // block the admit/resume fallback registration
        rc_->waiter_frame = rc_->resuming_frame;
    }

    // Run the off-lane signal bitset ON-LANE (audit Finding 1). Called at the top of
    // drain_step_reentrant. Everything here walks `live`/mutates `seal` while the worker holds
    // Running, so it is race-free; the off-lane caller only set an atomic bit + woke us.
    void apply_lane_actions(std::uint32_t acts) noexcept {
        ReCore& c = *rc_;
        if (acts & ReCore::kStopAll)  // 007 forced-stop: fire every in-flight cooperative token
            for (auto& f : c.live) f->stop.request_stop();
        if ((acts & ReCore::kEscalateCancel) &&
            c.seal.load(std::memory_order_relaxed) == SealState::Draining) {
            c.seal.store(SealState::Cancelling, std::memory_order_release);  // 015 bounded quiescence
            fire_cancel_all();
        }
    }

    // quiesce(Cancel) step 2 + the bounded-quiescence escalation (run on-lane via apply_lane_actions):
    // request every in-flight handler's cooperative stop_token and move every suspended sibling to
    // `ready` so the lane resumes them — each observes stop_requested and unwinds (co_return),
    // completing its reply cell. The waiter is never in `suspended`, so it is untouched here.
    void fire_cancel_all() noexcept {
        ReCore& c = *rc_;
        for (auto& f : c.live) f->stop.request_stop();
        std::lock_guard<std::mutex> lk(c.mtx);
        while (!c.suspended.empty()) {
            c.ready.push_back(c.suspended.front());
            c.suspended.pop_front();
        }
    }

    // Clear the seal (QuiescenceGuard destruction, step 5). Runs on the lane inside the waiter
    // handler; the drain loop then resumes admitting queued messages in FIFO order.
    void release_seal() noexcept {
        if (!rc_) return;
        rc_->seal.store(SealState::Open, std::memory_order_release);
        rc_->drain_deadline_ns.store(0, std::memory_order_release);  // disarm the watchdog
    }

    // Lane-side: the resuming frame just suspended at `co_await act.async_suspend()`.
    void on_async_suspend() noexcept {
        rc_->resuming_suspended = true;
        register_suspended(rc_->resuming_frame);
    }
    void register_suspended(ReFrame* f) noexcept {
        std::lock_guard<std::mutex> lk(rc_->mtx);
        rc_->suspended.push_back(f);
    }
    [[nodiscard]] bool has_pending_ready() noexcept {
        std::lock_guard<std::mutex> lk(rc_->mtx);
        return !rc_->ready.empty();
    }
    [[nodiscard]] ReFrame* pop_ready() noexcept {
        std::lock_guard<std::mutex> lk(rc_->mtx);
        if (rc_->ready.empty()) return nullptr;
        ReFrame* f = rc_->ready.front();
        rc_->ready.pop_front();
        return f;
    }

    // ========================================================================================
    // 007-Failure-and-Supervision / ADR-009 — the COLD supervision path. Every method here runs
    // only on a fault; none touches the zero-cost success path. `[[gnu::cold]]` keeps them off the
    // hot instruction cache (D3's `[[gnu::cold]] supervise()`, adopted).
    // ========================================================================================

    // The default (production) clock: the canonical PAL monotonic clock (018/019 — CLOCK_BOOTTIME
    // class, counts suspend). Routed through pal::now(), NOT steady_clock directly, so the restart
    // window and 022 deadline-shedding measure time in the SAME clock domain as Descriptor::
    // deadline_ns; a bare steady_clock here would freeze across suspend and diverge from deadlines.
    static std::int64_t real_steady_ns(void*) noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   pal::now().time_since_epoch())
            .count();
    }

    // Nanoseconds from the INJECTABLE clock (014 §virtual clock): the MaxRestarts sliding window
    // (cold, failure path) and deadline-aware shedding (022) read time through here so the 014
    // SimEngine can substitute a deterministic virtual clock for the host steady clock. Without
    // injection this is the real steady clock (== pal::clock, the basis of Descriptor::deadline_ns).
    [[nodiscard]] std::int64_t now_ns() const noexcept { return clock_fn_(clock_ctx_); }

    // Record a message's failure outcome at the SINGLE reclamation join point (007): Running→
    // Completed, hand the error to the dead-letter sink, then reclaim — which runs the payload dtor,
    // so an unanswered `ask`'s Responder fails its reply cell (reply-before-teardown, ADR-009 S2).
    [[gnu::cold]] void dead_letter_and_reclaim(Descriptor* d, error e) noexcept {
        ++dead_letters_;
        if (metrics_) metrics_->dead_letters.inc();  // lane-only (every call site is drain-owned)
        d->complete();
        dead_letter_(d, e);
        reclaim_(d);
    }

    // Sequential fault entry. The actor is at a quiescent point (its one in-flight message faulted),
    // so the decision applies synchronously.
    [[gnu::cold]] void on_fault_sequential(Descriptor* d, FailureSource src) noexcept {
        ++faults_;
        dead_letter_and_reclaim(d, error{errc::supervised_stop, "handler_fault"});  // outcome first
        if (src == FailureSource::Deadline || src == FailureSource::Cancellation)
            return;  // TRANSIENT carve-out (ADR-009 C5): Resume, no restart charge
        switch (sup_.decision) {
            case SupervisionDirective::Resume: return;          // keep state; message dropped
            case SupervisionDirective::Restart: do_restart(); return;
            case SupervisionDirective::Stop: do_stop(); return;
            case SupervisionDirective::Escalate: do_escalate(); return;
        }
    }

    // Reentrant fault entry (a suspended/started sibling faulted). Record THIS sibling's outcome,
    // drop it from the in-flight set, then apply the decision — Restart runs quiesce(Cancel) and the
    // reconstruct is deferred until the siblings drain (finish_restart_if_drained).
    [[gnu::cold]] void on_fault_reentrant(ReFrame* f, FailureSource src) noexcept {
        ++faults_;
        if (f->h) {
            f->h.destroy();  // faulted async frame is at final_suspend (done) ⇒ destroyable
            f->h = {};
        }
        dead_letter_and_reclaim(f->desc, error{errc::supervised_stop, "handler_fault"});
        remove_live(f);
        if (src == FailureSource::Deadline || src == FailureSource::Cancellation) {
            finish_restart_if_drained();
            return;  // TRANSIENT carve-out
        }
        switch (sup_.decision) {
            case SupervisionDirective::Resume: break;
            case SupervisionDirective::Restart: do_restart_reentrant(); break;
            case SupervisionDirective::Stop: do_stop(); break;
            case SupervisionDirective::Escalate: do_escalate(); break;
        }
        finish_restart_if_drained();
    }

    // Restart dispatch. Sequential reconstructs synchronously; Reentrant opens a restart episode.
    [[gnu::cold]] void do_restart() noexcept {
        if (rc_) {
            do_restart_reentrant();
            return;
        }
        if (!charge_restart()) {
            do_escalate();  // budget exhausted → escalate (poison already dead-lettered)
            return;
        }
        ++restarts_total_;
        if (metrics_) metrics_->restarts.inc();  // lane-only (fault path is drain-owned)
        reconstruct_now();
    }

    // Reentrant Restart = quiesce(Cancel) (015): seal admission, fire in-flight siblings' stop_tokens,
    // then reconstruct once they have all drained (finish_restart_if_drained). The restart-episode
    // marker keeps a MaxRestarts bound honest across concurrently-faulting siblings (ADR-009 S3).
    [[gnu::cold]] void do_restart_reentrant() noexcept {
#ifndef QUARK_SUPERVISION_NO_EPISODE_MARKER
        if (rc_->restarting) return;  // absorbed into the in-progress episode: no charge, no nest
#endif
        if (!charge_restart()) {
            do_escalate();
            return;
        }
        ++restarts_total_;
        if (metrics_) metrics_->restarts.inc();  // lane-only (fault path is drain-owned)
        rc_->restarting = true;
        rc_->seal.store(SealState::Cancelling, std::memory_order_release);  // seal: admit nothing (015)
        fire_cancel_all();                  // fire siblings' stop_tokens + move them to `ready`
        finish_restart_if_drained();        // reconstruct now if there were no other siblings
    }

    // Reconstruct when the in-flight set has drained to empty (reentrant) — the seal is released
    // AFTER every sibling's reply cell has completed (ADR-009 S2), so the lane resumes admitting the
    // survivor mailbox in FIFO order onto fresh state.
    [[gnu::cold]] void finish_restart_if_drained() noexcept {
        if (!rc_ || !rc_->restarting) return;
        if (stopped_) {
            rc_->restarting = false;  // an escalate-to-Stop mid-episode wins over reconstruct
            return;
        }
        if (!rc_->live.empty()) return;  // siblings still unwinding
        reconstruct_now();
        rc_->restarting = false;
        rc_->seal.store(SealState::Open, std::memory_order_release);  // release the seal (015 step 5)
        rc_->drain_deadline_ns.store(0, std::memory_order_release);   // disarm the watchdog
    }

    // MaxRestarts<N, Within<window>> sliding-window budget. Returns false on exhaustion.
    [[gnu::cold]] bool charge_restart() noexcept {
        if (sup_.window_ns > 0) {
            const std::int64_t now = now_ns();
            // `window_open_` is a DEDICATED "no window yet" flag — do NOT overload `window_start_ns_
            // == 0`, because 0 is a legitimate timestamp (the 014 virtual clock starts at 0, and any
            // monotonic epoch could too), which would make every restart look like a fresh window and
            // the budget would never deplete (bug the 014 sim exposed).
            if (!window_open_ || (now - window_start_ns_) > sup_.window_ns) {
                window_start_ns_ = now;
                window_open_ = true;
                restart_count_ = 0;  // window elapsed ⇒ the budget refills
            }
        }
        ++restart_count_;
        return restart_count_ <= sup_.max_restarts;
    }

    // 007 §Stop: deactivate — remaining/future messages dead-letter (drain-loop `stopped_` gate);
    // fire cooperative stops so in-flight reentrant siblings unwind.
    [[gnu::cold]] void do_stop() noexcept {
        stopped_ = true;
        stop_src_.request_stop();
        if (rc_) {
            rc_->restarting = false;
            for (auto& fr : rc_->live) fr->stop.request_stop();
        }
    }

    // 007 §Escalate: TELL the supervisor's lane (seam), then the default node-supervisor action —
    // Stop the actor + dead-letter survivors.
    [[gnu::cold]] void do_escalate() noexcept {
        ++escalations_;
        if (escalate_sink_.fn)
            escalate_sink_.fn(self_, error{errc::supervised_stop, "escalate"}, escalate_sink_.ctx);
        do_stop();
    }

    // Reconstruct actor state in place via the engine-supplied factory (null ⇒ assert-intact no-op).
    [[gnu::cold]] void reconstruct_now() noexcept { reconstruct_(self_); }

    void* self_;
    DispatchTable table_;
    ReclaimSink reclaim_;
    // Injectable time source (014). Default: real steady clock; SimEngine overrides via set_clock().
    ClockFn clock_fn_ = &real_steady_ns;
    void* clock_ctx_ = nullptr;
    ExecStateCell exec_{};
    Mailbox mailbox_{};
    MessageContext current_ctx_{};
    std::stop_source stop_src_{};
    // 001 Sequential parked-frame seam — single in-flight for Sequential (one message at a time).
    std::coroutine_handle<> parked_frame_{};
    Descriptor* parked_desc_ = nullptr;
    // 015 reentrant core — null for Sequential (zero cost); cold-allocated for Reentrant/MaxConc<N>.
    std::unique_ptr<ReCore> rc_{};
    // 007/ADR-009 supervision state — all COLD (touched only on a fault). Zero success-path cost.
    SupervisionPolicy sup_{};
    ReconstructSink reconstruct_{};
    DeadLetterSink dead_letter_{};
    EscalationSink escalate_sink_{};
    ShardCounters* metrics_ = nullptr;  // 009: this activation's shard block, wired by set_metrics()
    bool stopped_ = false;
    std::uint32_t restart_count_ = 0;     // restarts charged in the current MaxRestarts window
    std::int64_t window_start_ns_ = 0;    // start of the current window (valid iff window_open_)
    bool window_open_ = false;            // a window has been started (0 is a real timestamp, not a sentinel)
    std::uint32_t restarts_total_ = 0;    // total restarts driven (observability / tests)
    std::uint32_t escalations_ = 0;       // total escalations (observability / tests)
    std::uint32_t faults_ = 0;            // total handler faults contained (observability / tests)
    std::uint32_t dead_letters_ = 0;      // total messages routed to dead-letter
};

inline void QuiescenceGuard::release() noexcept {
    if (act_) {
        act_->release_seal();
        act_ = nullptr;
    }
}

}  // namespace quark
