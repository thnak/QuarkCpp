// Implements 014-Testing-Model §Deterministic simulation (`SimEngine`) + §Fault injection +
// §Invariant checking — a drop-in, SINGLE-THREADED, deterministic driver for the SAME production
// actor/dispatch/supervision code (001/007/008/015). There is NO parallel mock universe: SimEngine
// owns real `Activation`s and drives the real `try_acquire → drain_step → close_out` protocol; it
// differs from the production `Engine` (002) ONLY in the *scheduler* (a seeded single-thread picker
// instead of an N-worker run-queue) and the *clock* (a virtual clock instead of `pal::now()`).
//
// ============================================================================================
// HOW DETERMINISM IS GUARANTEED (014 §Alternatives considered — "controllable nondeterminism")
//
// The production engine has exactly two nondeterminism sources: (1) which worker thread runs which
// runnable activation, and (2) wall-clock timing of timers/deadlines. SimEngine removes BOTH:
//
//   * ONE THREAD. `run_until_idle()` runs on the calling thread. There is no real concurrency, so
//     the single-executor invariant holds trivially and there is nothing to be flaky about. A
//     Sequential activation driven single-threaded is byte-for-byte reproducible by construction.
//
//   * SEEDED CHOICE MODEL. The scheduler's only free choice is *which runnable activation runs
//     next*. SimEngine models that as `rng_() % runnable_.size()` over an ORDERED runnable vector
//     whose contents evolve only through deterministic edges (a `post` that drove Idle→Scheduled
//     appends; a close-out to Idle removes). Same seed ⇒ identical pick sequence ⇒ identical message
//     interleaving. Per-actor message order is NEVER a choice — the mailbox is FIFO and the sim never
//     reorders one actor's stream (CONVENTIONS + 014 §Invariant checking: FIFO is CHECKED, not a
//     tunable). Cross-actor interleaving is the whole of the seeded schedule.
//
//   * VIRTUAL CLOCK. `advance(d)` ticks a `TimingWheel` (011) by `d / tick_ns` and fires due timers
//     as plain `tell`s — no `pal::now()`, no sleeping. Firing order within a tick is the wheel's
//     deterministic bucket order; the follow-on delivery is the same seeded schedule. Same seed +
//     same advance calls ⇒ identical timer-driven runs.
//
// Each `run_until_idle()` STEP delivers exactly ONE message (drain budget 1) to the picked actor,
// so the recorded step trace IS the message interleaving. Two runs with equal seeds produce a
// byte-identical `trace()`; a different seed (almost surely) diverges — the reproducibility proof.
//
// ============================================================================================
// FAULT INJECTION (014 §Fault injection). All seeded/explicit, so a discovered bug replays exactly:
//   * transport DROP — `drop_next_to(id, nth)` / `set_random_drop(p)`: a message is dropped at the
//     post seam (its payload dtor runs → an unanswered `ask` cell is failed, never hangs). Exercises
//     006 dead-letter + the ask reply-before-teardown path deterministically.
//   * handler FAULT — `arm_handler_fault(id, nth)`: a COOPERATING handler calls
//     `consume_handler_fault(id)` and throws on the scheduled dispatch, driving the REAL 007
//     supervision decision (Restart/Stop/Escalate) + 015 quiescence through the production guard.
//   * transport DELAY — via `schedule_after` on the virtual clock (a timer-driven `tell`): a
//     deterministic, order-preserving delay. (Reordering ONE actor's stream is intentionally NOT a
//     fault — it would violate the FIFO invariant the sim checks.)
//
// SCOPE / SEAMS. Node failures / partitions (010) and store faults (012) are documented seams — the
// same seeded-decision surface extends to them once those subsystems land. Deadline-timeout ENFORCE-
// MENT (a fired deadline → `FailureSource::Deadline`) rides the 018 clock seam; the virtual clock +
// timer wheel here are the mechanism it will hang off. A full sim PAL backend (019) that swaps
// `pal::clock`/`pal::now()` for this virtual clock under the standalone `TimerService` is the clean
// long-term shape; this header realizes the sim scheduler + clock directly on the public seams.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor_ref.hpp"   // LocalRouter, ActorRef, PostCourier, Schedulable
#include "quark/core/dispatch.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metadata.hpp"    // actor_id_of, make_reconstruct_sink
#include "quark/core/policies.hpp"    // max_concurrency_of
#include "quark/core/supervision.hpp" // supervision_of
#include "quark/core/timer_wheel.hpp"
#include "quark/detail/message_pool.hpp"

namespace quark {

// One recorded scheduler step (014 §expose the schedule). The vector of these IS the deterministic
// interleaving trace: same seed ⇒ byte-identical sequence. Trivially comparable.
struct SimStep {
    std::uint64_t index = 0;            // 0-based step number
    ActorId actor{};                   // which activation was run this step
    std::uint32_t runnable_before = 0;  // size of the runnable set at pick time
    std::uint32_t pick = 0;             // chosen index into the runnable set (the seeded choice)
    friend bool operator==(const SimStep&, const SimStep&) = default;
};

// A minimal cancellation token over a sim-scheduled timer (011 lazy gen-gated cancel).
class SimTimerHandle {
public:
    SimTimerHandle() noexcept = default;
    SimTimerHandle(detail::TimerEntry* e, std::uint64_t gen) noexcept : entry_(e), gen_(gen) {}
    void cancel() const noexcept {
        if (entry_ != nullptr && entry_->active && entry_->gen == gen_) entry_->cancelled = true;
    }
    [[nodiscard]] bool valid() const noexcept { return entry_ != nullptr; }

private:
    detail::TimerEntry* entry_ = nullptr;
    std::uint64_t gen_ = 0;
};

class SimEngine {
public:
    struct Config {
        std::uint64_t seed = 0;
        std::chrono::nanoseconds tick = std::chrono::milliseconds(1);  // virtual-clock granularity
        std::size_t pool_capacity = 4096;    // pre-sized message pool (cold growth past this)
        std::size_t timer_capacity = 1024;   // pre-sized timer-entry pool
    };

    explicit SimEngine(std::uint64_t seed) : SimEngine(Config{seed}) {}
    explicit SimEngine(Config cfg)
        : cfg_(cfg),
          tick_ns_(cfg.tick.count() <= 0 ? 1 : static_cast<std::uint64_t>(cfg.tick.count())),
          rng_(cfg.seed),
          fault_rng_(splitmix(cfg.seed) ^ 0x9E37'79B9'7F4A'7C15ULL),
          msg_pool_(cfg.pool_capacity),
          timer_pool_(cfg.timer_capacity),
          router_(courier(), msg_pool_) {}

    SimEngine(const SimEngine&) = delete;
    SimEngine& operator=(const SimEngine&) = delete;

    // --- Spawn / address (014 §same public API) ------------------------------------------------
    // Construct the actor + its Activation from the SAME 007/008 wiring the production engine uses
    // (supervision_of<A>() + make_reconstruct_sink<A>() so a Restart reconstructs true fresh state),
    // reclaim through the sim message pool, and register it. Returns a live ActorRef<A>.
    template <class A>
    [[nodiscard]] ActorRef<A> spawn(std::uint64_t key) {
        static_assert(is_actor<A>, "SimEngine::spawn<A>: A must derive from quark::Actor<A, ...>");
        const ActorId id = actor_id_of<A>(key);
        auto node = std::make_unique<SimNode>();
        node->id = id;
        node->actor = new A();  // sim owns the instance for its lifetime (freed in dtor)
        node->destroy = +[](void* p) noexcept { delete static_cast<A*>(p); };
        node->act = std::make_unique<Activation>(node->actor, A::dispatch_table(), msg_pool_.sink(),
                                                 max_concurrency_of<A>(), supervision_of<A>());
        node->act->set_reconstruct(make_reconstruct_sink<A>());
        node->act->set_dead_letter_sink(DeadLetterSink{&SimEngine::dead_letter_thunk, this});
        // Drive the activation's supervision-window + deadline-shed time off the VIRTUAL clock (014
        // §virtual clock) — the fix for the wall-clock leak the audit proved: a MaxRestarts<N,Within<W>>
        // outcome now depends on advance() calls (seeded), not host wall time.
        node->act->set_clock(
            +[](void* ctx) noexcept -> std::int64_t {
                return static_cast<const SimEngine*>(ctx)->now_ns();
            },
            this);
        node->sched.activation = node->act.get();
        node->sched.shard = 0;
        node->sched.budget = 1;  // one message per step ⇒ fine-grained, seeded interleaving
        SimNode* raw = node.get();
        by_id_[id] = raw;
        sched_to_node_[&raw->sched] = raw;  // courier hands back Schedulable* → recover the node
        nodes_.push_back(std::move(node));
        return router_.get<A>(key);
    }

    template <class A>
    [[nodiscard]] ActorRef<A> get(std::uint64_t key) noexcept {
        return router_.get<A>(key);
    }
    template <class A>
    [[nodiscard]] A& actor(std::uint64_t key) noexcept {
        return *static_cast<A*>(by_id_.at(actor_id_of<A>(key))->actor);
    }
    template <class A>
    [[nodiscard]] Activation& activation(std::uint64_t key) noexcept {
        return *by_id_.at(actor_id_of<A>(key))->act;
    }
    [[nodiscard]] LocalRouter& router() noexcept { return router_; }

    // --- The deterministic drain (014 §deterministic drain) ------------------------------------
    // Run seeded steps until no activation is runnable. Returns the number of steps taken. Each step
    // picks ONE runnable activation by the seed and delivers ONE message to it.
    std::uint64_t run_until_idle() {
        std::uint64_t steps = 0;
        while (!runnable_.empty()) {
            const std::uint32_t n = static_cast<std::uint32_t>(runnable_.size());
            const std::uint32_t pick = static_cast<std::uint32_t>(rng_() % n);
            SimNode* node = runnable_[pick];
            trace_.push_back(SimStep{step_counter_, node->id, n, pick});
            trace_digest_ = mix_digest(trace_digest_, node->id.hash(), pick, n);
            ++step_counter_;
            ++steps;
            step_node(node, pick);
        }
        return steps;
    }

    // --- Virtual clock (014 §virtual clock; 011) -----------------------------------------------
    void advance(std::chrono::nanoseconds by) {
        const std::uint64_t ns = static_cast<std::uint64_t>(by.count() <= 0 ? 0 : by.count());
        const std::uint64_t ticks = ns / tick_ns_;
        for (std::uint64_t i = 0; i < ticks; ++i)
            wheel_.tick([this](detail::TimerEntry* e) noexcept { on_expire(e); });
        now_ns_ += ticks * tick_ns_;
        run_until_idle();  // deliver the timer-driven `tell`s deterministically
    }
    [[nodiscard]] std::int64_t now_ns() const noexcept { return static_cast<std::int64_t>(now_ns_); }
    [[nodiscard]] std::uint64_t now_ticks() const noexcept { return wheel_.now_ticks(); }

    // Deterministic one-shot timer: `tell` `msg` to `ref` after `delay` of VIRTUAL time. A firing is
    // a plain 006 `tell` (single-executor preserved), 0 heap alloc on the fire path (inline closure).
    template <class A, class M>
    [[nodiscard]] SimTimerHandle schedule_after(const ActorRef<A>& ref, std::chrono::nanoseconds delay,
                                                M msg) {
        return arm<A, M>(ref, delay, std::chrono::nanoseconds::zero(), std::move(msg));
    }
    template <class A, class M>
    [[nodiscard]] SimTimerHandle schedule_every(const ActorRef<A>& ref, std::chrono::nanoseconds period,
                                                M msg) {
        return arm<A, M>(ref, period, period, std::move(msg));
    }

    // --- Fault injection (014 §Fault injection) — all seeded/explicit, so bugs replay ----------
    // Transport drop: drop the `nth` (1-based) message delivered to `id`, at the post seam.
    void drop_next_to(ActorId id, std::uint64_t nth) { drops_[id].push_back(nth); }
    // Seeded random transport drop with probability `p` in [0,1], driven by the fault PRNG (kept
    // SEPARATE from the schedule PRNG so enabling drops does not perturb the interleaving of the
    // messages that DO get through — both remain fully deterministic under the same seed).
    void set_random_drop(double p) noexcept { drop_prob_ = p < 0 ? 0 : (p > 1 ? 1 : p); }

    // Handler fault: the `nth` (1-based) dispatch to `id` for which `consume_handler_fault(id)`
    // returns true (a cooperating handler then throws → real 007 supervision).
    void arm_handler_fault(ActorId id, std::uint64_t nth) { hfaults_[id] = HandlerFault{nth, 0}; }
    [[nodiscard]] bool consume_handler_fault(ActorId id) noexcept {
        auto it = hfaults_.find(id);
        if (it == hfaults_.end()) return false;
        ++it->second.seen;
        return it->second.seen == it->second.nth;
    }

    // --- The schedule / counters (014 §expose the schedule + §Invariant checking) --------------
    [[nodiscard]] const std::vector<SimStep>& trace() const noexcept { return trace_; }
    [[nodiscard]] std::uint64_t trace_digest() const noexcept { return trace_digest_; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return step_counter_; }
    [[nodiscard]] std::uint64_t posted() const noexcept { return posted_; }        // transport-accepted
    [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }   // dispatched (1/step)
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }       // transport-dropped
    [[nodiscard]] std::uint64_t dead_letters() const noexcept { return dead_letters_; }

    // 014 §Invariant checking — "no lost message": every transport-accepted message reached a
    // terminal dispatch (Completed / dead-letter) once the sim is idle. Holds by construction (we
    // drive to `runnable_.empty()`), asserted here so a test can turn it into a property check.
    [[nodiscard]] bool no_lost_message() const noexcept {
        return runnable_.empty() && posted_ == delivered_;
    }

    ~SimEngine() {
        for (auto& n : nodes_)
            if (n->actor && n->destroy) n->destroy(n->actor);
    }

private:
    // Container_of node: the Schedulable is the first member, so a Schedulable* the courier hands
    // back round-trips to its owning SimNode (matches the 002 Engine's Schedulable ownership model).
    struct SimNode {
        Schedulable sched{};                 // MUST be first (offset-0 round-trip)
        ActorId id{};
        void* actor = nullptr;               // owned instance (type-erased; freed via `destroy`)
        void (*destroy)(void*) noexcept = nullptr;
        std::unique_ptr<Activation> act{};
        bool queued = false;                 // membership flag for `runnable_` (dedup the wake edge)
        std::uint64_t post_seq = 0;          // transport delivery counter (drop targeting)
    };

    // Recover the owning node from the Schedulable* the courier hands back. A map lookup (not a
    // container_of cast) keeps this warning-clean and standard-layout-agnostic; the sim is not a
    // perf path (the production Engine owns the Schedulable directly, ADR-010).
    [[nodiscard]] SimNode* node_of(Schedulable* s) noexcept { return sched_to_node_.at(s); }

    // The sim-local courier (mirrors Engine::post_courier but posts into the single-thread runnable
    // set instead of waking a worker). LocalRouter/ActorRef post through this, so the send API is
    // byte-identical to production.
    [[nodiscard]] PostCourier courier() noexcept {
        return PostCourier{this, &SimEngine::resolve_thunk, &SimEngine::post_thunk};
    }
    static Schedulable* resolve_thunk(void* eng, ActorId id) noexcept {
        auto* self = static_cast<SimEngine*>(eng);
        auto it = self->by_id_.find(id);
        return it == self->by_id_.end() ? nullptr : &it->second->sched;
    }
    static bool post_thunk(void* eng, Schedulable* s, Descriptor* d) noexcept {
        return static_cast<SimEngine*>(eng)->post(s, d);
    }

    // Producer hot path (sim variant of Engine::post): apply fault injection, then enqueue + carry
    // the Idle→Scheduled wake edge onto the runnable set.
    bool post(Schedulable* s, Descriptor* d) noexcept {
        SimNode* node = node_of(s);
        ++node->post_seq;
        if (should_drop(node)) {
            ++dropped_;
            msg_pool_.reclaim(d);  // runs the payload dtor → fails an unanswered ask cell (no hang)
            return false;
        }
        ++posted_;
        const bool wake = s->activation->post(d);
        if (wake) mark_runnable(node);
        return wake;
    }

    [[nodiscard]] bool should_drop(SimNode* node) noexcept {
        auto it = drops_.find(node->id);
        if (it != drops_.end())
            for (std::uint64_t nth : it->second)
                if (nth == node->post_seq) return true;
        if (drop_prob_ > 0.0) {
            std::uniform_real_distribution<double> u(0.0, 1.0);
            if (u(fault_rng_) < drop_prob_) return true;
        }
        return false;
    }

    void mark_runnable(SimNode* node) {
        if (!node->queued) {
            node->queued = true;
            runnable_.push_back(node);
        }
    }
    void remove_runnable(std::uint32_t idx) noexcept {
        runnable_[idx]->queued = false;
        runnable_[idx] = runnable_.back();
        runnable_.pop_back();
    }

    // Run exactly one drain step (budget 1 ⇒ one message) on the picked node, then reconcile its
    // runnable membership with its exec-state. Mirrors Engine::run_activation, single-threaded.
    void step_node(SimNode* node, std::uint32_t pick) {
        Activation& act = *node->act;
        if (!act.try_acquire()) {  // not Scheduled (should not happen: runnable ⇒ Scheduled) — drop it
            remove_runnable(pick);
            return;
        }
        for (;;) {
            const Activation::DrainOutcome o = act.drain_step(1);
            if (o == Activation::DrainOutcome::DrainedEmpty) {
                // Single-threaded: no producer can race the close-out window, so this relinquishes to
                // Idle. (The `close_out()==true` re-acquire branch is unreachable here; loop defensively.)
                if (act.close_out()) continue;
                remove_runnable(pick);
                return;
            }
            if (o == Activation::DrainOutcome::BudgetExhausted) {
                ++delivered_;                 // budget 1 ⇒ exactly one message dispatched this step
                act.yield_to_scheduled();     // Running→Scheduled; stays runnable for the next step
                return;
            }
            if (o == Activation::DrainOutcome::Busy) {
                continue;  // no concurrent producer single-threaded ⇒ resolves immediately
            }
            // Suspended: an async handler parked. In the pure-sync sim there is no carrier to
            // complete it, so it leaves the runnable set (a full async/fiber sim carrier is a seam).
            ++delivered_;
            remove_runnable(pick);
            return;
        }
    }

    // --- Timer arming (011; single-writer, no mutex — the whole sim is one thread) --------------
    template <class A, class M>
    struct BoundSend {
        ActorRef<A> ref;
        M msg;
    };
    template <class A, class M>
    static void fire_bound(void* p) noexcept {
        auto* b = static_cast<BoundSend<A, M>*>(p);
        b->ref.tell(b->msg);
    }
    template <class A, class M>
    static void destroy_bound(void* p) noexcept {
        static_cast<BoundSend<A, M>*>(p)->~BoundSend<A, M>();
    }
    template <class A, class M>
    [[nodiscard]] SimTimerHandle arm(const ActorRef<A>& ref, std::chrono::nanoseconds delay,
                                     std::chrono::nanoseconds period, M msg) {
        using Bound = BoundSend<A, std::remove_cvref_t<M>>;
        static_assert(sizeof(Bound) <= detail::TimerEntry::kInlineCap,
                      "timer payload too large for the inline entry slot");
        detail::TimerEntry* e = timer_pool_.acquire();
        e->expiry_ticks = wheel_.now_ticks() + ticks_ceil(delay);
        e->period_ticks = period > std::chrono::nanoseconds::zero() ? ticks_ceil(period) : 0;
        e->deadline_ns = static_cast<std::int64_t>(now_ns_) + delay.count();
        ::new (e->storage) Bound{ref, std::move(msg)};
        e->fire_fn = &fire_bound<A, std::remove_cvref_t<M>>;
        e->destroy_fn = &destroy_bound<A, std::remove_cvref_t<M>>;
        wheel_.insert(e);
        return SimTimerHandle{e, e->gen};
    }
    [[nodiscard]] std::uint64_t ticks_ceil(std::chrono::nanoseconds d) const noexcept {
        const std::int64_t ns = d.count() <= 0 ? 0 : d.count();
        const std::uint64_t t = (static_cast<std::uint64_t>(ns) + tick_ns_ - 1) / tick_ns_;
        return t == 0 ? 1 : t;
    }
    void on_expire(detail::TimerEntry* e) noexcept {
        if (e->cancelled) {
            recycle(e);
            return;
        }
        e->fire_fn(e->storage);  // a `tell` → post → mark_runnable (delivered on the next drain)
        if (e->period_ticks != 0) {
            e->expiry_ticks += e->period_ticks;
            e->deadline_ns += static_cast<std::int64_t>(e->period_ticks * tick_ns_);
            wheel_.insert(e);
        } else {
            recycle(e);
        }
    }
    void recycle(detail::TimerEntry* e) noexcept {
        if (e->destroy_fn != nullptr) e->destroy_fn(e->storage);
        timer_pool_.release(e);
    }

    static void dead_letter_thunk(Descriptor*, error, void* ctx) noexcept {
        ++static_cast<SimEngine*>(ctx)->dead_letters_;
    }

    // splitmix64 — derive an independent, deterministic fault-PRNG seed from the schedule seed.
    static std::uint64_t splitmix(std::uint64_t x) noexcept {
        x += 0x9E37'79B9'7F4A'7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return x ^ (x >> 31);
    }
    static std::uint64_t mix_digest(std::uint64_t acc, std::uint64_t a, std::uint32_t b,
                                    std::uint32_t c) noexcept {
        acc = splitmix(acc ^ a);
        acc = splitmix(acc ^ (static_cast<std::uint64_t>(b) << 32 | c));
        return acc;
    }

    struct HandlerFault {
        std::uint64_t nth = 0;
        std::uint64_t seen = 0;
    };

    Config cfg_;
    std::uint64_t tick_ns_;
    std::mt19937_64 rng_;         // the SCHEDULE PRNG — the seeded choice model
    std::mt19937_64 fault_rng_;   // the FAULT PRNG — independent so faults don't perturb the schedule
    detail::MessagePool msg_pool_;
    detail::TimerEntryPool timer_pool_;
    detail::TimingWheel wheel_{};
    LocalRouter router_;
    std::uint64_t now_ns_ = 0;

    std::vector<std::unique_ptr<SimNode>> nodes_;      // stable-address activation owners
    std::unordered_map<ActorId, SimNode*> by_id_;      // addressing registry (resolve)
    std::unordered_map<Schedulable*, SimNode*> sched_to_node_;  // Schedulable* → owning node
    std::vector<SimNode*> runnable_;                   // ordered runnable set (the seeded pick domain)

    std::vector<SimStep> trace_;
    std::uint64_t trace_digest_ = 0xCBF2'9CE4'8422'2325ULL;  // FNV offset basis
    std::uint64_t step_counter_ = 0;
    std::uint64_t posted_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t dead_letters_ = 0;

    std::unordered_map<ActorId, std::vector<std::uint64_t>> drops_;  // explicit transport drops
    std::unordered_map<ActorId, HandlerFault> hfaults_;              // armed handler faults
    double drop_prob_ = 0.0;                                          // seeded random drop rate
};

}  // namespace quark
