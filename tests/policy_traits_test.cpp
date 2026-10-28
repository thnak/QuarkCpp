// Tests 005-Developer-Model §Policy catalog + §Metadata — the compile-time trait extractors over an
// actor's `Policies...` pack. Everything load-bearing here is a `static_assert`: band, budget,
// reentrancy, and max-concurrency must be read correctly FROM the policy list, with the right
// DEFAULTS when a policy is absent, and the pack must be recovered without any edit to actor.hpp.
#include <cassert>
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/actor_ref.hpp"  // Ask<Q,R>
#include "quark/core/policies.hpp"

using namespace quark;

namespace {

struct Ping {};
struct Query {};

// --- Bare actor: no policies at all. Fully valid, Sequential by construction. --------------------
struct Bare : Actor<Bare> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- Explicit Sequential + scheduling policies. -------------------------------------------------
struct Ordered : Actor<Ordered, Sequential, Priority<0>, DrainBudget<64>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- Reentrant + a concurrency cap + a low band. Order of policies is irrelevant. ---------------
struct Worker : Actor<Worker, DrainBudget<8>, Reentrant, MaxConcurrency<4>, Priority<2>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- Lifecycle + placement surface (declared-only; extracted where cheap). ----------------------
struct Sticky : Actor<Sticky, KeepAlive, Placement<HashById>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

struct Ephemeral : Actor<Ephemeral, IdleTimeout<5000>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ============================================================================================
// Pack recovery — no actor.hpp edit; deduced through the public Actor base.
// ============================================================================================
static_assert(is_actor<Bare>);
static_assert(is_actor<Worker>);
static_assert(!is_actor<Ping>);  // a plain message type is not an actor
static_assert(policies_of<Bare>::size == 0);
static_assert(policies_of<Ordered>::size == 3);
static_assert(policies_of<Worker>::size == 4);

// ============================================================================================
// Defaults when a policy is ABSENT.
// ============================================================================================
static_assert(priority_band_of<Bare>() == 0, "no Priority ⇒ band 0 (default)");
static_assert(drain_budget_of<Bare>() == 0, "no DrainBudget ⇒ 0 (⇒ engine default)");
static_assert(!is_reentrant_v<Bare>, "no Reentrant/MaxConcurrency ⇒ not reentrant");
static_assert(max_concurrency_of<Bare>() == 1, "Sequential default ⇒ 1 in-flight");
static_assert(!keeps_alive_v<Bare>);
static_assert(idle_timeout_ms_of<Bare>() == 0);

// ============================================================================================
// Explicit scheduling policies are read correctly.
// ============================================================================================
static_assert(priority_band_of<Ordered>() == 0);
static_assert(drain_budget_of<Ordered>() == 64, "DrainBudget<64> ⇒ 64");
static_assert(!is_reentrant_v<Ordered>, "explicit Sequential ⇒ not reentrant");
static_assert(max_concurrency_of<Ordered>() == 1);
static_assert(has_policy_v<Sequential, Ordered>);

// ============================================================================================
// Reentrant + MaxConcurrency + a non-zero band, in any order.
// ============================================================================================
static_assert(priority_band_of<Worker>() == 2, "Priority<2> ⇒ band 2");
static_assert(drain_budget_of<Worker>() == 8, "DrainBudget<8> ⇒ 8");
static_assert(is_reentrant_v<Worker>, "Reentrant ⇒ reentrant");
static_assert(max_concurrency_of<Worker>() == 4, "MaxConcurrency<4> ⇒ 4");
static_assert(has_policy_v<Reentrant, Worker>);
static_assert(!has_policy_v<Sequential, Worker>);

// ============================================================================================
// Lifecycle extractors.
// ============================================================================================
static_assert(keeps_alive_v<Sticky>, "KeepAlive present");
static_assert(idle_timeout_ms_of<Sticky>() == 0, "KeepAlive ⇒ no timeout");
static_assert(!keeps_alive_v<Ephemeral>);
static_assert(idle_timeout_ms_of<Ephemeral>() == 5000, "IdleTimeout<5000> ⇒ 5000ms");

// ============================================================================================
// Validation passes for well-formed actors (its teeth — the conflict static_asserts — fire on
// instantiation; a conflicting actor would fail to compile, which is the intended behavior).
// ============================================================================================
static_assert(validate_actor_policies<Bare>());
static_assert(validate_actor_policies<Ordered>());
static_assert(validate_actor_policies<Worker>());
static_assert(validate_actor_policies<Sticky>());

// A reentrant actor still keyed and dispatched normally — trait extraction is orthogonal to the
// protocol, which enumerates Ask envelopes just the same.
struct Service : Actor<Service, Reentrant, Priority<1>> {
    using protocol = Protocol<Ask<Query, int>>;
    void handle(const Ask<Query, int>& m) noexcept { m.respond(1); }
};
static_assert(is_reentrant_v<Service>);
static_assert(max_concurrency_of<Service>() == 0, "Reentrant w/o cap ⇒ 0 (unbounded sentinel)");
static_assert(priority_band_of<Service>() == 1);

}  // namespace

int main() {
    // All assertions are compile-time; reaching main means they held.
    std::printf("policy_traits_test: OK\n");
    return 0;
}
