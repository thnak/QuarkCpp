// Implements 005-Developer-Model §Metadata / §Validation — the REGISTRATION PATH that closes the
// ADR-010 seam: the engine's `register_activation` takes a band + budget EXPLICITLY, but 005 says
// the developer declares INTENT (`Priority<P>`, `DrainBudget<N>`) and the engine RESOLVES it. This
// header is that resolver.
//
//   register_actor<Order>(engine, id, activation);   // resolves band+budget from Order's policies
//
// It is a free function (typed on the actor, consistent with 006's `ActorRef<A>` / `router.get<A>`)
// — no edit to `engine.hpp` is needed because `register_activation` already accepts the resolved
// values; 005 simply supplies them from `policies_of<A>` instead of the caller passing them by hand.
//
// The caller still owns the `Activation` (and its ReclaimSink → DescriptorPool wiring) exactly as
// the 001/002 seam documents; this only maps type → (shard, band, budget) and hands back the
// `Schedulable` the producer hot path uses (mirroring `register_activation`'s contract).
#pragma once

#include <cstdint>

#include "quark/core/activation.hpp"
#include "quark/core/actor_ref.hpp"  // actor_id_of<A> (006 keying, consistent with ActorRef)
#include "quark/core/engine.hpp"     // Engine<Policy>, register_activation, DrainBudget (canonical)
#include "quark/core/ids.hpp"
#include "quark/core/policies.hpp"

namespace quark {

// Register `A`'s activation, resolving its `Priority<P>` → band and `DrainBudget<N>` → budget from
// the actor's policy pack. `band` 0 = highest. Returns the engine-owned `Schedulable*`.
//
// Fail-fast Validation (005 §Validation, Strict = compile error; Relaxed is an 008 seam):
//   * policy conflicts (Sequential+Reentrant, …) via `validate_actor_policies<A>()`;
//   * the resolved band must fit the engine's bands — `Priority<P>` with P ≥ K is rejected here
//     rather than silently clamped (ADR-010 Validation (i): distinct priority classes ≤ K).
template <class A, class Policy>
Schedulable* register_actor(Engine<Policy>& engine, ActorId id, Activation& act) {
    static_assert(is_actor<A>, "register_actor<A>: A must derive from quark::Actor<A, ...>");
    static_assert(validate_actor_policies<A>(), "register_actor<A>: actor policy validation failed");
    static_assert(priority_band_of<A>() < Policy::bands,
                  "register_actor<A>: Priority<P> exceeds the engine's PriorityBands<K> "
                  "(P must be < K) — ADR-010 §Validation");
    return engine.register_activation(id, act, static_cast<std::uint16_t>(priority_band_of<A>()),
                                      drain_budget_of<A>());
}

// Convenience overload keyed by the actor's instance key (identical keying to `LocalRouter::get<A>`
// / `ActorRef<A>`): `register_actor<Order>(engine, 7, act)` ↔ `router.get<Order>(7)`.
template <class A, class Policy>
Schedulable* register_actor(Engine<Policy>& engine, std::uint64_t key, Activation& act) {
    return register_actor<A>(engine, actor_id_of<A>(key), act);
}

}  // namespace quark
