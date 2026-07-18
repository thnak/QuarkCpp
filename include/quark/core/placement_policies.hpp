// Implements 025-Placement-Policies-and-Stateless-Workers §Part B (runtime) — the DETERMINISTIC
// placement resolver for STATEFUL actors: a strategy (`HashById`/`Explicit`/`Custom<F>`) run over the
// eligible node subset produced by the modifiers (`Require`/`Prefer`/`Weighted`/`LocalFirst`/
// `Affinity`/`AntiAffinity`). Design + proofs pinned by ADR-013 (the accepted `Weighted` form
// `score = wₙ/(−ln H)`, proportional at the multinomial floor, minimal-churn on reweight) and ADR-011
// (Gate B churn: 0 bins between UNCHANGED-weight nodes).
//
// This header holds the RUNTIME resolution; the TAG surface + compile-time extraction/validation live
// in policies.hpp (`placement_of<A>`, `Require`/`Prefer`/…). It EXTENDS placement.hpp's uniform HRW
// (`place_hash`) to (a) an ELIGIBLE SUBSET and (b) a WEIGHTED score, and COMPOSES with 026's
// `VirtualBins` O(1) cache for the unconstrained `HashById` path.
//
// DETERMINISM INVARIANT (load-bearing, 025): a stateful placement policy is a PURE FUNCTION of
// (ActorId, gossiped membership+capability set). It may NOT read live load. Enforced structurally:
// `Custom<F>` is handed ONLY the CapabilityView; nothing here can observe queue depth / CPU. Live-load
// routing exists ONLY for the identity-less `Stateless` pool (stateless_pool.hpp).
//
// 026 COMPOSITION (per 026 §"Interaction with 025"):
//   * unconstrained `HashById` (no modifiers) → the O(1) `VirtualBins` bin table (when supplied).
//   * `Require`/`Weighted`/`Prefer` → O(eligible) ≤ N EXACT HRW over the survivors — the honest core.
//   * `AntiAffinity` / per-actor spread → EXACT per-actor HRW, BYPASSING the bin cache (bins quantize
//     placement and would collapse a deliberate spread — 026 line 78).
//
// ============================================================================================
// SEAMS LEFT EXPLICIT (documented, NOT implemented here — each names the downstream owner):
//   * PER-ELIGIBILITY-CLASS VirtualBins TABLE — the O(1) accelerator for CONSTRAINED placement (one
//     bin table per distinct capability/eligibility class, refilled per epoch). 026's optimization
//     over the O(eligible) exact fallback that IS implemented here → 026. This header proves the exact
//     oracle; the per-class table is a cache layered on it (identical winners by construction).
//   * CLUSTER-WIDE STATELESS routing + gossiped (approximate, one-round-stale) load signal → 010/021
//     (see stateless_pool.hpp). Placement modifiers here select the ELIGIBLE nodes for such routing;
//     the cross-node hop + the stale load read are the seam.
//   * Affinity target RE-PLACEMENT on membership change (if `Affinity<A>`'s target A moves, does the
//     affine actor follow atomically?) → 010/021 handoff (025 open question).
// ============================================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/cluster_topology.hpp"  // VirtualBins — the O(1) unconstrained-HashById cache
#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/metadata.hpp"    // type_key_of<A> — the Affinity/AntiAffinity target's identity
#include "quark/core/placement.hpp"   // place_hash / rendezvous_weight — the uniform HRW we generalize
#include "quark/core/policies.hpp"    // Placement<S, Ms...>, Require/Prefer/Weighted/..., placement_of

namespace quark {

// ============================================================================================
// The proportional log-WRH `Weighted` score (ADR-013). `H(key,node) ∈ (0,1)`: the splitmix64
// rendezvous weight (placement.hpp, the SAME mixer HashById/VirtualBins use) mapped to the OPEN unit
// interval; `score = weightₙ / (−ln H)`. Argmax wins. NOT `weight·H` (ADR-011/012 proved that
// non-proportional). Winner is proportional at the multinomial floor and moves minimally on reweight.
// ============================================================================================

// splitmix64 output → a double in the OPEN interval (0,1). Top 53 bits + 0.5, scaled by 2⁻⁵³: the
// numerator lies in [0.5, 2⁵³−0.5], so the result is strictly inside (0,1) — never exactly 0 or 1, so
// `−ln H` is finite and strictly positive (guards the score denominator).
[[nodiscard]] inline double hrw_unit_open(std::uint64_t h) noexcept {
    const std::uint64_t m = h >> 11;  // 53-bit mantissa
    return (static_cast<double>(m) + 0.5) * (1.0 / 9007199254740992.0);  // /2^53
}

// The proportional log-WRH score for node `n` on key `key_hash` with relative capacity `w` (ADR-013).
[[nodiscard]] inline double weighted_hrw_score(NodeId n, std::uint64_t key_hash, double w) noexcept {
    const double u = hrw_unit_open(rendezvous_weight(n, key_hash));  // H ∈ (0,1)
    const double denom = -std::log(u);                              // > 0
    return w / denom;
}

// The weighted-HRW winner for `key_hash` over `cand`, weights read from the annotated view. Highest
// score wins; on a tie the higher NodeId::value wins (total order ⇒ byte-identical on every node).
[[nodiscard]] inline std::optional<NodeId> place_weighted(std::uint64_t key_hash,
                                                          std::span<const NodeId> cand,
                                                          const CapabilityView& v) noexcept {
    bool have = false;
    NodeId best{};
    double best_s = 0.0;
    for (NodeId n : cand) {
        const double s = weighted_hrw_score(n, key_hash, v.weight_of(n));
        if (!have || s > best_s || (s == best_s && n.value > best.value)) {
            best = n;
            best_s = s;
            have = true;
        }
    }
    return have ? std::optional<NodeId>(best) : std::nullopt;
}

namespace detail {

// --- Modifier matchers (runtime resolution dispatch). ------------------------------------------
template <class T>
struct is_require : std::false_type {};
template <class... C>
struct is_require<::quark::Require<C...>> : std::true_type {};

template <class T>
struct is_prefer : std::false_type {};
template <class... C>
struct is_prefer<::quark::Prefer<C...>> : std::true_type {};

// AND of the capability predicates carried by a Require<>/Prefer<> (empty pack ⇒ vacuously true).
template <class T>
struct caps_pred {
    [[nodiscard]] static bool eligible(const NodeCapabilities&) noexcept { return true; }
};
template <class... C>
struct caps_pred<::quark::Require<C...>> {
    [[nodiscard]] static bool eligible(const NodeCapabilities& c) noexcept {
        return (C::eligible(c) && ... && true);
    }
};
template <class... C>
struct caps_pred<::quark::Prefer<C...>> {
    [[nodiscard]] static bool eligible(const NodeCapabilities& c) noexcept {
        return (C::eligible(c) && ... && true);
    }
};

template <class T>
struct is_affinity : std::false_type {};
template <class A>
struct is_affinity<::quark::Affinity<A>> : std::true_type {};
template <class T>
struct affinity_target;
template <class A>
struct affinity_target<::quark::Affinity<A>> {
    using type = A;
};

template <class T>
struct is_antiaffinity : std::false_type {};
template <class A>
struct is_antiaffinity<::quark::AntiAffinity<A>> : std::true_type {};
template <class T>
struct antiaffinity_target;
template <class A>
struct antiaffinity_target<::quark::AntiAffinity<A>> {
    using type = A;
};

template <class T>
struct is_custom : std::false_type {};
template <class F>
struct is_custom<::quark::Custom<F>> : std::true_type {};

[[nodiscard]] inline bool contains(const std::vector<NodeId>& v, NodeId n) noexcept {
    for (NodeId x : v)
        if (x == n) return true;
    return false;
}

// The node where the Affinity/AntiAffinity target actor `A` (same instance key) is placed. EXACT
// per-actor HRW over the full node set — never the quantized bin cache (026 §025: spread must bypass
// bins). Uses the target's own HashById base placement (its identity, not its modifiers).
template <class A>
[[nodiscard]] inline std::optional<NodeId> target_node(ActorId id, const CapabilityView& v) noexcept {
    const ActorId tid{type_key_of<A>(), id.key};
    return place_hash(tid.hash(), v.nodes());
}

// --- Per-modifier appliers (each a no-op unless the modifier matches). --------------------------
template <class M>
inline void apply_require(std::vector<NodeId>& elig, const CapabilityView& v) {
    if constexpr (is_require<M>::value) {
        std::erase_if(elig, [&](NodeId n) { return !caps_pred<M>::eligible(v.capabilities_of(n)); });
    }
}
template <class M>
inline void apply_prefer(std::vector<NodeId>& pref, const CapabilityView& v, bool& any) {
    if constexpr (is_prefer<M>::value) {
        any = true;
        std::erase_if(pref, [&](NodeId n) { return !caps_pred<M>::eligible(v.capabilities_of(n)); });
    }
}
template <class M>
inline void apply_affinity(std::vector<NodeId>& elig, ActorId id, const CapabilityView& v) {
    if constexpr (is_affinity<M>::value) {
        const auto node = target_node<typename affinity_target<M>::type>(id, v);
        if (node && contains(elig, *node)) elig.assign(1, *node);  // co-locate when eligible
    }
}
template <class M>
inline void apply_antiaffinity(std::vector<NodeId>& elig, ActorId id, const CapabilityView& v) {
    if constexpr (is_antiaffinity<M>::value) {
        const auto node = target_node<typename antiaffinity_target<M>::type>(id, v);
        if (node) std::erase(elig, *node);  // never land on the target's node
    }
}

}  // namespace detail

// ============================================================================================
// The resolver. `resolve_placement_impl(Placement<S, Ms...>{}, …)`: modifiers narrow/rank the eligible
// set, then the single strategy S chooses. Returns `result<NodeId>` — an empty eligible set (a
// `Require` no node satisfies) is a 007-style error, NOT UB. `caller` (0 ⇒ none) drives `LocalFirst`;
// `bins` (optional) is the 026 cache used only for the unconstrained HashById path; `explicit_target`
// supplies the node for `Explicit`.
// ============================================================================================
template <class S, class... Ms>
[[nodiscard]] inline result<NodeId> resolve_placement_impl(Placement<S, Ms...>, ActorId id,
                                                           const CapabilityView& v, NodeId caller,
                                                           const VirtualBins* bins,
                                                           std::optional<NodeId> explicit_target) {
    if constexpr (std::is_same_v<S, Explicit>) {
        // Caller-specified node (006 addressing). No auto-resolution.
        if (explicit_target) return *explicit_target;
        return fail(errc::not_found, "Placement<Explicit>: no explicit target node supplied");
    } else if constexpr (detail::is_custom<S>::value) {
        // Custom<F>: user deterministic policy, handed ONLY the view (determinism invariant, 025).
        using F = typename S::fn;
        return F{}(id, v);
    } else {
        static_assert(std::is_same_v<S, HashById>,
                      "unknown placement strategy (expected HashById / Explicit / Custom<F>)");
        constexpr bool weighted = (std::is_same_v<Ms, Weighted> || ... || false);
        constexpr std::size_t nmods = sizeof...(Ms);

        // 026 composition: unconstrained + unweighted HashById may use the O(1) bin cache.
        if constexpr (nmods == 0) {
            if (bins != nullptr) {
                const auto o = bins->owner_of(id);
                return o ? result<NodeId>(*o) : fail(errc::unavailable, "empty cluster");
            }
        }

        if (v.empty()) return fail(errc::unavailable, "empty membership — no node to place onto");

        // Eligible set (control plane; a vector alloc is fine off the drain hot path).
        std::vector<NodeId> elig(v.nodes().begin(), v.nodes().end());
        (detail::apply_require<Ms>(elig, v), ...);      // hard capability filters
        (detail::apply_affinity<Ms>(elig, id, v), ...);      // co-locate (exact per-actor HRW)
        (detail::apply_antiaffinity<Ms>(elig, id, v), ...);  // spread (exact per-actor HRW, bypass bins)

        if (elig.empty())
            return fail(errc::not_found,
                        "no eligible node satisfies the placement constraints (Require/Affinity)");

        // LocalFirst — strongest, per-caller latency optimization (still deterministic per caller).
        constexpr bool has_lf = (std::is_same_v<Ms, LocalFirst> || ... || false);
        if constexpr (has_lf) {
            if (caller.value != 0 && detail::contains(elig, caller)) return result<NodeId>(caller);
        }

        // Prefer — restrict to nodes satisfying ALL Prefer predicates, if any qualify; else fall back.
        std::vector<NodeId> pref = elig;
        bool any_prefer = false;
        (detail::apply_prefer<Ms>(pref, v, any_prefer), ...);
        const std::span<const NodeId> cand = (any_prefer && !pref.empty())
                                                 ? std::span<const NodeId>(pref)
                                                 : std::span<const NodeId>(elig);

        // Exactly one strategy chooses from the survivors: weighted or uniform HRW.
        const std::optional<NodeId> w =
            weighted ? place_weighted(id.hash(), cand, v) : place_hash(id.hash(), cand);
        return w ? result<NodeId>(*w) : fail(errc::unavailable, "no winner among eligible nodes");
    }
}

// Resolve a placement policy TYPE against the annotated view. `resolve_placement<Placement<HashById,
// Require<Gpu>>>(id, view)`; `place_actor<A>` resolves `A`'s declared policy.
template <class Placement_>
[[nodiscard]] inline result<NodeId> resolve_placement(ActorId id, const CapabilityView& v,
                                                      NodeId caller = {},
                                                      const VirtualBins* bins = nullptr,
                                                      std::optional<NodeId> explicit_target = std::nullopt) {
    return resolve_placement_impl(Placement_{}, id, v, caller, bins, explicit_target);
}

// Resolve actor `A`'s DECLARED placement policy (`placement_of<A>`) — a plain `Actor<A, Sequential>`
// gets `Placement<HashById>` and routes through the unchanged HRW / bin-cache path (zero-cost-when-unused).
template <class A>
[[nodiscard]] inline result<NodeId> place_actor(ActorId id, const CapabilityView& v, NodeId caller = {},
                                                const VirtualBins* bins = nullptr,
                                                std::optional<NodeId> explicit_target = std::nullopt) {
    return resolve_placement<placement_of<A>>(id, v, caller, bins, explicit_target);
}

}  // namespace quark
