// Tests 017-Delivery-Guarantees §"The three levels" — the `Delivery<Level>` policy surface: the
// compile-time extractor (`delivery_of` / `delivery_level_of`), the FREE `AtMostOnce` default
// (zero-cost-when-unused), and the Validation rule "EffectivelyOnce requires a Persistent actor".
// Everything load-bearing here is a `static_assert`: the policy is read correctly FROM the pack, a
// plain actor resolves to `AtMostOnce` with NO effectively-once machinery instantiated, and a
// non-persistent EffectivelyOnce actor is a compile-time config error.
#include <cstdio>

#include "quark/core/actor.hpp"
#include "quark/core/persistence.hpp"  // Persistent, EventSourced, Snapshot, PersistMode
#include "quark/core/policies.hpp"

using namespace quark;

namespace {

struct Ping {};

// --- Plain actor: no Delivery<> at all → the free AtMostOnce default. ----------------------------
struct Plain : Actor<Plain, Sequential> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- Explicit AtLeastOnce (idempotent-handler contract; no persistence required). ---------------
struct BestEffort : Actor<BestEffort, Sequential, Delivery<DeliveryLevel::AtLeastOnce>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- EffectivelyOnce over a Persistent<EventSourced> actor — the well-formed CP shape. -----------
struct Order : Actor<Order, Sequential, Persistent<EventSourced, PersistMode::Sync>,
                     Delivery<DeliveryLevel::EffectivelyOnce>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- EffectivelyOnce over a Persistent<Snapshot> actor — also persistent, also well-formed. ------
struct Wallet : Actor<Wallet, Sequential, Persistent<Snapshot, PersistMode::Sync>,
                      Delivery<DeliveryLevel::EffectivelyOnce>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// --- MISUSE: EffectivelyOnce on a NON-persistent actor. We DO NOT instantiate
//     validate_actor_policies<Bad> (that would hard-fail the build); instead we assert the
//     constexpr predicate reports the violation — the same technique the 025 conflict tests use. ---
struct Bad : Actor<Bad, Sequential, Delivery<DeliveryLevel::EffectivelyOnce>> {
    using protocol = Protocol<Ping>;
    void handle(const Ping&) noexcept {}
};

// ============================================================================================
// Extraction + defaults.
// ============================================================================================
static_assert(delivery_of<Plain> == DeliveryLevel::AtMostOnce, "no Delivery<> ⇒ AtMostOnce (default)");
static_assert(delivery_level_of<Plain>() == DeliveryLevel::AtMostOnce);
static_assert(delivery_of<BestEffort> == DeliveryLevel::AtLeastOnce, "Delivery<AtLeastOnce> read back");
static_assert(delivery_of<Order> == DeliveryLevel::EffectivelyOnce, "Delivery<EffectivelyOnce> read back");
static_assert(delivery_of<Wallet> == DeliveryLevel::EffectivelyOnce);

// ============================================================================================
// ZERO-COST-WHEN-UNUSED. A plain actor lists no Delivery<> — its recovered pack is unchanged, its
// delivery level is the free default, and it is NOT persistent, so NONE of the effectively-once
// machinery (dedup/outbox/watermark, delivery.hpp) is reachable for it. The extractor perturbs no
// pre-existing trait: band/budget/reentrancy/lifecycle all resolve exactly as before.
// ============================================================================================
static_assert(policies_of<Plain>::size == 1, "Delivery extractor adds no phantom policy to a plain actor");
static_assert(delivery_of<Plain> == DeliveryLevel::AtMostOnce);
static_assert(!is_persistent_v<Plain>);
static_assert(priority_band_of<Plain>() == 0, "delivery extraction does not perturb the priority default");
static_assert(drain_budget_of<Plain>() == 0);
static_assert(max_concurrency_of<Plain>() == 1);
static_assert(!is_reentrant_v<Plain>);

// ============================================================================================
// Persistence predicate + the EffectivelyOnce-requires-Persistent Validation rule.
// ============================================================================================
static_assert(is_persistent_v<Order>);
static_assert(is_persistent_v<Wallet>);
static_assert(!is_persistent_v<Plain>);
static_assert(!is_persistent_v<BestEffort>);

// The rule as a predicate: VIOLATED only for a non-persistent EffectivelyOnce actor.
static_assert(effectively_once_needs_persistence<Bad>(), "EffectivelyOnce w/o Persistent ⇒ violation");
static_assert(!effectively_once_needs_persistence<Order>(), "EffectivelyOnce + Persistent ⇒ OK");
static_assert(!effectively_once_needs_persistence<Plain>(), "AtMostOnce never violates the rule");
static_assert(!effectively_once_needs_persistence<BestEffort>(), "AtLeastOnce never violates the rule");

// Validation PASSES (its teeth are the internal static_asserts) for every well-formed actor.
static_assert(validate_actor_policies<Plain>());
static_assert(validate_actor_policies<BestEffort>());
static_assert(validate_actor_policies<Order>());
static_assert(validate_actor_policies<Wallet>());

}  // namespace

int main() {
    // All assertions are compile-time; reaching main means they held.
    std::printf("delivery_policy_traits_test: OK\n");
    return 0;
}
