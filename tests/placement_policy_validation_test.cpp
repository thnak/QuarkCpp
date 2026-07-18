// Tests 025-Placement-Policies-and-Stateless-Workers §Validation (008) + the ZERO-COST-WHEN-UNUSED
// contract. All load-bearing checks here are COMPILE-TIME (static_assert): the new Placement-modifier /
// Stateless machinery must EXTEND policies.hpp additively — a plain `Actor<T, Sequential>` must keep the
// unchanged stateful HashById path and instantiate NONE of the modifier/pool types. Validation rejects
// the three 025 conflicts (Stateless+Explicit, Stateless+persistence, empty Require<>) — proven via the
// constexpr rule PREDICATES (so we can assert a rule FIRES without tripping its hard static_assert).
#include <cstdio>
#include <type_traits>

#include "quark/core/actor.hpp"
#include "quark/core/capabilities.hpp"
#include "quark/core/persistence.hpp"
#include "quark/core/policies.hpp"

using namespace quark;

namespace {

struct Ping {};
using Gpu = HasFlag<"gpu">;
using SameZone = HasLabel<"zone", "eu-west-1">;

// ---- Zero-cost baseline: a plain Sequential actor, no placement/pool policies. -----------------
struct Plain : Actor<Plain, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ---- Constrained + weighted stateful actor (the spec's GpuDecoder shape). ----------------------
struct GpuDecoder
    : Actor<GpuDecoder, Sequential, Placement<HashById, Require<Gpu>, Prefer<SameZone>, Weighted>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ---- Stateless pool actor. ---------------------------------------------------------------------
struct Validator : Actor<Validator, Stateless<8>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
struct WideValidator : Actor<WideValidator, Stateless<4, ClusterWide, Require<Gpu>>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ---- The three CONFLICTS (defined only to read their rule predicates — never validated/registered). --
struct BadStatelessExplicit : Actor<BadStatelessExplicit, Stateless<4>, Placement<Explicit>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
struct BadStatelessPersist
    : Actor<BadStatelessPersist, Stateless<4>, Persistent<EventSourced, PersistMode::Sync>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};
struct BadEmptyRequire : Actor<BadEmptyRequire, Placement<HashById, Require<>>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ============================================================================================
// ZERO-COST-WHEN-UNUSED: the plain actor's placement resolves to the UNCHANGED default `Placement
// <HashById>` — no modifier types, not stateless, not weighted, not explicit. Byte-for-byte the
// pre-025 stateful single-activation path (the extractors below return their pre-025 defaults).
// ============================================================================================
static_assert(std::is_same_v<placement_of<Plain>, Placement<HashById>>,
              "a plain Sequential actor defaults to the unchanged Placement<HashById>");
static_assert(!is_stateless_v<Plain>, "a plain actor is not a stateless pool");
static_assert(!placement_is_weighted_v<Plain>, "a plain actor is not weighted");
static_assert(!placement_is_explicit_v<Plain>, "a plain actor is not explicit");
static_assert(policy_traits_of<Plain>::placement_facts::modifier_count == 0,
              "a plain actor carries zero placement modifiers");
// The pre-025 extractors are unperturbed for the plain actor.
static_assert(max_concurrency_of<Plain>() == 1 && priority_band_of<Plain>() == 0 &&
                  drain_budget_of<Plain>() == 0 && !is_reentrant_v<Plain>,
              "025 additions do not perturb the pre-existing stateful extractors");
static_assert(validate_actor_policies<Plain>(), "the plain actor still validates");

// ============================================================================================
// Extraction of the new surface is correct.
// ============================================================================================
static_assert(std::is_same_v<placement_of<GpuDecoder>,
                             Placement<HashById, Require<Gpu>, Prefer<SameZone>, Weighted>>);
static_assert(placement_is_weighted_v<GpuDecoder>, "Weighted modifier extracted");
static_assert(!is_stateless_v<GpuDecoder>);
static_assert(policy_traits_of<GpuDecoder>::placement_facts::modifier_count == 3);
static_assert(validate_actor_policies<GpuDecoder>(), "a constrained+weighted actor validates");

static_assert(is_stateless_v<Validator> && stateless_size_of<Validator>() == 8);
static_assert(!stateless_cluster_wide_v<Validator>, "a plain Stateless<N> is local-only");
static_assert(validate_actor_policies<Validator>());

static_assert(is_stateless_v<WideValidator> && stateless_size_of<WideValidator>() == 4);
static_assert(stateless_cluster_wide_v<WideValidator>, "Stateless<N, ClusterWide> opts into 010/021 seam");
static_assert(validate_actor_policies<WideValidator>(), "ClusterWide + Require validates");

// ============================================================================================
// The three 025 conflicts are DETECTED (each predicate is true iff the rule is violated). These would
// each fail validate_actor_policies<>() at compile time — we assert the DETECTOR fires without tripping
// the hard static_assert, and confirm a well-formed actor does NOT trip it.
// ============================================================================================
static_assert(stateless_explicit_conflict<BadStatelessExplicit>(),
              "Stateless + Placement<Explicit> is a detected conflict (025 §Validation)");
static_assert(!stateless_explicit_conflict<Validator>());

static_assert(stateless_persistence_conflict<BadStatelessPersist>(),
              "Stateless + Persistent<EventSourced> is a detected conflict (025/012)");
static_assert(!stateless_persistence_conflict<Validator>());

static_assert(empty_require_present<BadEmptyRequire>(),
              "empty Require<> is a detected config error (025 §Validation)");
static_assert(!empty_require_present<GpuDecoder>(), "a non-empty Require<Gpu> is not flagged");

}  // namespace

int main() {
    // Everything above is compile-time; reaching main means every rule held.
    std::printf("placement_policy_validation_test: OK\n");
    return 0;
}
