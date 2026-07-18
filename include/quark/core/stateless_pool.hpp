// Implements 025-Placement-Policies-and-Stateless-Workers §Part C — the `Stateless<N>` worker pool.
// The relaxation is proven by ADR-011 Gate C (exactly-once under concurrency: lost=dup=torn=0, TSan/
// ASan clean, shared-state control races, pool 1.5–2.8× > N hand-rolled actors).
//
// A `Stateless<N>` pool = up to N LOCAL activations, LOAD-ROUTED (least-loaded power-of-two-choices /
// round-robin), spawn-on-demand, NO identity, NO `ActorId→node` pin, NO per-(sender,receiver) FIFO to
// the pool. It relaxes exactly three core invariants (at-most-one-activation, stable placement, FIFO-
// to-a-pool) and KEEPS the one that matters: **single-executor PER activation**. Each pool activation
// is a plain `quark::Activation` (001/002/003) driven UNCHANGED — the engine is not weakened; there is
// simply no SHARED state across the pool to protect. A faulting activation is discarded and its message
// dead-lettered (007) — no Restart/quiesce/fencing (there is no state to reconstruct).
//
// WHY ROUTING MAY READ LIVE LOAD: a stateless pool has no identity to pin, so the determinism invariant
// (025 Part B — stateful placement is a pure function of gossiped state, never load) does NOT apply.
// The load signal is the per-activation mailbox DEPTH (009), tracked by the Activation's own governance
// accounting (enqueued − drained) — the same 009 signal, reused, not a new mechanism.
//
// WHAT THIS FILE IS (025 Part C std-only core):
//   * StatelessPool<A> — N `Activation`s over N `A` instances + a LOCAL load-balancing router. Holds no
//     threads (the caller's workers drive the activations, exactly like the 002 scheduler drives one).
//   * Route::LeastLoaded — power-of-two-choices on mailbox depth (ADR-011 Gate C's router).
//   * Route::RoundRobin  — the simple alternative (025 §Routing).
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (documented, NOT implemented here — each names the downstream owner):
//   * CLUSTER-WIDE `Stateless<N, ClusterWide>` — eligible-node selection (Part B modifiers) then
//     least-loaded among ELIGIBLE nodes, cross-node routing preferring local, over a GOSSIPED
//     (approximate, one-round-stale) load signal → 010 / 021. This pool is LOCAL-only; the network hop
//     and the stale load read are the seam.
//   * ADAPTIVE POOL SIZING / autoscaling N — `when` to change N (or add nodes) is the external control
//     loop over 009 (022/024), not the engine (025 §Non-goals). N is a fixed bounded resource here.
//   * IdleTimeout POOL SHRINK — `IdleTimeout<Ms>` (policies.hpp) reaping idle activations → 013/022
//     lifecycle. This pool eager-allocates N slots (the std-only form); lazy spawn-on-demand + idle
//     reap is the lifecycle optimization layered on top (`used()` exposes which slots have been routed).
// ============================================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "quark/core/activation.hpp"
#include "quark/core/actor.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/detail/hash.hpp"  // splitmix64 — the router's choice mixer (no std::random_device)

namespace quark {

// The local routing discipline (025 §Routing and load-awareness).
enum class PoolRoute : std::uint8_t {
    LeastLoaded = 0,  // power-of-two-choices on mailbox depth (ADR-011 Gate C)
    RoundRobin = 1,   // simple rotation (025 §Routing)
};

// ============================================================================================
// StatelessPool<A> — N single-executor activations + a LOCAL load-balancing router. Reuses the
// unchanged `Activation`: each slot owns an `A` instance and an `Activation` over it, with governance
// enabled ONLY so the Activation tracks its own mailbox depth (the 009 load signal the router reads).
// No cross-slot shared state — each activation's `A` is its own; the pool holds only routing metadata.
// ============================================================================================
template <class A>
class StatelessPool {
public:
    // One pool activation: an `A` instance + its `Activation`, heap-pinned so the Activation's
    // `self` pointer and the slot's atomics never move (Activation is non-movable).
    struct Slot {
        // Default-init (no braces): an actor with a public base is an AGGREGATE, and brace-init would
        // try to initialize the protected `Actor` base subobject directly from here. `A actor;` uses
        // A's own (implicit) default ctor, which legally reaches the protected base.
        A actor;
        std::unique_ptr<Activation> act;
        std::atomic<std::uint8_t> used{0};  // set once the slot has been routed to (≤N-used accounting)
    };

    // `n` = pool size N. `route` = the local load-balancing discipline. `dl` = the 007 dead-letter sink
    // a faulting activation's message routes to (a stateless fault is discarded + dead-lettered).
    explicit StatelessPool(std::size_t n, PoolRoute route = PoolRoute::LeastLoaded,
                           DeadLetterSink dl = {}, std::uint64_t salt = 0x9E3779B97F4A7C15ULL)
        : route_(route), salt_(salt) {
        slots_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            auto s = std::make_unique<Slot>();
            s->act = std::make_unique<Activation>(&s->actor, A::dispatch_table());
            // Governance with an UNBOUNDED bound (static_bound == 0): never sheds/blocks; its sole
            // purpose here is that the Activation tracks depth = enqueued − drained (the 009 load
            // signal the router reads via mailbox_depth()). This does NOT weaken the drain — the
            // governed sequential path is the verbatim single-in-flight drain (022).
            s->act->enable_governance(Activation::GovernanceConfig{});
            // Stateless failure/lifecycle (025 §Failure): a faulting activation is DISCARDED and its
            // message dead-lettered (007) — NO Restart/quiesce/fencing (there is no state to rebuild).
            // `Stop` supervision realizes exactly that: the faulted message is dead-lettered and the
            // activation deactivates; the pool re-grows on demand (a fresh slot). The default engine
            // Restart is deliberately overridden here — a stateless pool does not reconstruct state.
            s->act->set_supervision(SupervisionPolicy{SupervisionDirective::Stop});
            if (dl.fn) s->act->set_dead_letter_sink(dl);
            slots_.push_back(std::move(s));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] Activation& activation(std::size_t i) noexcept { return *slots_[i]->act; }
    [[nodiscard]] A& actor(std::size_t i) noexcept { return slots_[i]->actor; }
    // Current queue depth of slot `i` (the 009 load signal): enqueued − drained.
    [[nodiscard]] std::uint64_t depth(std::size_t i) const noexcept {
        return slots_[i]->act->mailbox_depth();
    }
    // How many distinct slots have been routed to at least once (≤ N — the pool never exceeds N).
    [[nodiscard]] std::size_t used_count() const noexcept {
        std::size_t u = 0;
        for (const auto& s : slots_)
            if (s->used.load(std::memory_order_relaxed)) ++u;
        return u;
    }

    // Pick a slot per the routing discipline. LeastLoaded = power-of-two-choices: mix two slot indices
    // from splitmix64 (deterministic, NO std::random_device) and take the shallower mailbox — O(1),
    // scalable, and the ADR-011 Gate C router. RoundRobin = a relaxed rotation.
    [[nodiscard]] std::size_t route_slot() noexcept {
        const std::size_t n = slots_.size();
        const std::uint64_t t = seq_.fetch_add(1, std::memory_order_relaxed);
        if (route_ == PoolRoute::RoundRobin) return static_cast<std::size_t>(t % n);
        const std::uint64_t r = detail::splitmix64(t + salt_);
        const std::size_t a = static_cast<std::size_t>(r % n);
        const std::size_t b = static_cast<std::size_t>((r >> 32) % n);
        return depth(a) <= depth(b) ? a : b;
    }

    // Route + post a pre-stamped descriptor to the chosen activation. Returns the slot index. The
    // descriptor is enqueued on exactly ONE activation's mailbox → each message is handled by exactly
    // one activation (the exactly-once foundation). Ignores the wake edge — the caller's workers
    // busy-poll the acquire edge (the harness stands in for 002's targeted wakeup), as in
    // exec_single_executor_test.
    std::size_t post(Descriptor* d) noexcept {
        const std::size_t i = route_slot();
        return post_to(i, d);
    }

    // Post directly to slot `i`, bypassing the router (used by tests to pin a message to a specific
    // activation; also the hook a cluster-wide router would call after choosing an eligible node).
    std::size_t post_to(std::size_t i, Descriptor* d) noexcept {
        slots_[i]->used.store(1, std::memory_order_relaxed);
        (void)slots_[i]->act->post_governed(d);  // unbounded ⇒ always Admitted
        return i;
    }

    StatelessPool(const StatelessPool&) = delete;
    StatelessPool& operator=(const StatelessPool&) = delete;

private:
    std::vector<std::unique_ptr<Slot>> slots_;
    PoolRoute route_;
    std::uint64_t salt_;
    std::atomic<std::uint64_t> seq_{0};  // routing sequence (round-robin counter / p2c mixer input)
};

}  // namespace quark
