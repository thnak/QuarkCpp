// Implements 005-Developer-Model §Policy catalog + §Metadata — the actor-authoring policy TAGS and
// the compile-time TRAIT EXTRACTORS over an actor's `Policies...` pack.
//
// The developer declares INTENT as CRTP policy types on the `Actor` base
// (`class Order : public Actor<Order, Sequential, Priority<0>, DrainBudget<64>>`); this header turns
// that pack into engine-consumable metadata at COMPILE TIME — no RTTI, no reflection, no virtual,
// pure `if constexpr` / pack-fold traversal (CONVENTIONS: "no RTTI/reflection/virtual for policy").
//
// The pack is recovered WITHOUT editing `actor.hpp`: an actor `A` publicly derives from
// `Actor<A, Ps...>`, so binding an `A&` to `const Actor<D, Ps...>&` deduces `Ps...` (the standard
// detect-base-template-args idiom). New policy tags therefore compose by being LISTED — this header
// never needs to be edited to add one, and `actor.hpp` never needs to know they exist.
//
// SEAMS: `Reentrant` / `MaxConcurrency<N>` runtime semantics are 015 (this header only DECLARES the
// surface and EXTRACTS the declared intent — the base's runtime `is_reentrant` flag stays a 015
// seam). `Placement<…>` / `Stateless<N>` runtime is 025. Lifecycle knobs (`KeepAlive`,
// `IdleTimeout<Ms>`) are live-reconfigurable operational fields (013/ADR-008) — declared here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "quark/core/actor.hpp"  // Actor<Derived, Policies...> + `Sequential` (the default, reused)

namespace quark {

// Forward decls of the 012 persistence policy surface (canonical definitions in persistence.hpp).
// Declared (not included) so the reentrant-EventSourced guard below can pattern-match the tag by
// template-id WITHOUT this light actor-authoring header pulling in the persistence path — exactly
// the technique used for the forward-declared `DrainBudget<N>` (defined in engine.hpp). The opaque
// scoped-enum declaration and the default-arg-free template decl both match persistence.hpp's
// definitions, so including both headers in one TU stays well-formed.
enum class PersistMode : std::uint8_t;
struct EventSourced;
template <class Model, PersistMode Mode>
struct Persistent;

// ============================================================================================
// Policy catalog (005 §Policy catalog). Every policy is a TYPE; each slot has a default, so an
// actor with no policies (`Actor<Order>`) is fully valid and Sequential. Order is irrelevant.
// ============================================================================================

// --- Execution (005) — `Sequential` (default) lives in actor.hpp and is reused verbatim. --------

// Begin the next message while an async handler is suspended (015 runtime; here: declared surface).
struct Reentrant {};

// Cap in-flight handlers at N (015 runtime; here: declared surface + extracted bound).
template <std::size_t N>
struct MaxConcurrency {
    static constexpr std::size_t value = N;
};

// --- Scheduling (005 §Scheduling; ADR-010) ------------------------------------------------------

// Names a compile-time priority CLASS consumed by the engine-level `PriorityBands<K, Anti>` policy.
// `P` is the band index (0 = highest); startup resolves it against the engine's K bands — `P` must
// be < K (Validation, ADR-010). Band is CONSTANT per actor type (no per-message recompute), which
// is what makes per-actor mailbox FIFO hold by construction (ADR-010 C1).
template <std::size_t P>
struct Priority {
    static constexpr std::size_t value = P;
};

// DrainBudget<N> — max messages drained before yielding the lane (005 §Scheduling). Its CANONICAL
// definition lives in `engine.hpp` (the engine resolves it to a runtime `Schedulable::budget`); it
// is only FORWARD-DECLARED here so the actor-authoring extractors sit together without this
// light-weight header pulling in the whole engine. The two decls are the same template — including
// both `engine.hpp` and this header in one TU is well-formed. (Reported seam.)
template <std::uint32_t N>
struct DrainBudget;

// --- Placement (005 §Placement; 025 runtime) — declared surface only. ---------------------------
//
// 025 Part B generalizes placement from a single strategy to a STRATEGY + optional MODIFIERS, all
// resolved against the gossiped membership+capability set, all DETERMINISTIC. This header declares the
// tag surface + the compile-time EXTRACTORS / VALIDATION; the runtime resolution (eligible-subset HRW,
// the ADR-013 proportional log-WRH `Weighted` form, Affinity/AntiAffinity) lives in
// placement_policies.hpp — exactly as `HashById`/`Explicit` are declared here but `place()` runs in
// placement.hpp. Adding these tags perturbs NO existing extractor: a plain `Actor<T, Sequential>`
// declares no `Placement<>`/`Stateless<>`, so `placement_of` defaults to `Placement<HashById>` and the
// stateful single-activation path stays byte-for-byte unchanged (025 zero-cost-when-unused).

// --- Strategies (exactly one CHOOSES from the survivors) ---
struct HashById {};   // default strategy: stable HRW hash of ActorId → node → shard.
struct Explicit {};   // caller-specified shard/node.
// User deterministic policy: `F(ActorId, CapabilityView) → NodeId`. Handed ONLY the annotated view,
// never a live-load signal — the determinism invariant (025) enforced by the type of `F` (the resolver
// calls `F{}(id, view)` and nothing else is in scope).
template <class F>
struct Custom {
    using fn = F;
};

// --- Modifiers (filter / rank the eligible node set BEFORE the strategy chooses) ---
// Hard filter: eligible = nodes with ALL of Cap... . Empty eligible ⇒ placement error (007). An
// EMPTY `Require<>` (no caps) is a config error rejected at validation (025 §Validation).
template <class... Caps>
struct Require {};
// Soft rank: preferred-cap nodes rank first; fall back to the full eligible set if none qualify.
template <class... Caps>
struct Prefer {};
// Capacity-weighted HRW using each node's `weight` — the ADR-013 proportional log-WRH `w/(−ln H)`.
struct Weighted {};
// Prefer the calling node if it is eligible (per-caller latency optimization; still deterministic).
struct LocalFirst {};
// Co-locate with actor `A`'s placement (cache/locality).
template <class A>
struct Affinity {};
// Place away from actor `A`'s placement (fault isolation / spread). BYPASSES the 026 bin cache and
// computes exact per-actor HRW (bins quantize and would collapse a deliberate spread — 026 §025).
template <class A>
struct AntiAffinity {};

// Strategy = HashById (default) + zero or more modifiers. `Placement<HashById>` / `Placement<>` is the
// unchanged default; `Placement<HashById, Require<Gpu>, Prefer<SameZone>, Weighted>` composes.
template <class Strategy = HashById, class... Modifiers>
struct Placement {
    using strategy = Strategy;
};

// --- Stateless pool (025 Part C) ---
// `ClusterWide` opts a pool into cross-node routing (least-loaded among eligible nodes, gossiped/stale
// load) — a documented 010/021 seam; the default (absent) pool is LOCAL-only. Placement modifiers may
// ride the opts (`Stateless<N, ClusterWide, Require<Gpu>>`) to pick eligible nodes cluster-wide.
struct ClusterWide {};

// Pool of up to N local activations, load-routed, no identity (025 Part C; runtime = stateless_pool.hpp).
template <std::size_t N, class... Opts>
struct Stateless {
    static constexpr std::size_t value = N;
};

// --- Lifecycle (005 §Lifecycle; 013/ADR-008 operational knobs) — declared surface. --------------
struct KeepAlive {};  // never deactivate on idle (the `idle_ticks == 0` sentinel, 013).
template <std::uint64_t Ms>
struct IdleTimeout {
    static constexpr std::uint64_t ms = Ms;
};

// --- Delivery guarantees (017 §"The three levels") ----------------------------------------------
// The per-actor end-to-end delivery guarantee. `AtMostOnce` is the DEFAULT (free — no dedup/outbox/
// watermark machinery is instantiated for an actor that declares no `Delivery<>`), so a plain
// `Actor<T, Sequential>` resolves to `AtMostOnce` and its path stays byte-for-byte unchanged. The
// effectively-once MECHANISM (per-sender high-water-mark dedup + fenced atomic commit + post-commit
// transactional outbox) lives in `delivery.hpp` and is only pulled in when an actor opts into it;
// this header owns ONLY the policy TAG + its extractor + the compile-time validation rule.
enum class DeliveryLevel : std::uint8_t {
    AtMostOnce,       // deliver 0–1 times, no retry — free, requires nothing (the default)
    AtLeastOnce,      // deliver ≥1 time, retry + ack — handler must be idempotent (documented contract)
    EffectivelyOnce,  // effect (state + outbound) happens exactly once — requires Persistent + a CP store
};
template <DeliveryLevel L>
struct Delivery {
    static constexpr DeliveryLevel value = L;
};

// ============================================================================================
// Pack recovery + trait extraction (compile-time; no `actor.hpp` edit).
// ============================================================================================

// A carrier for a recovered policy pack.
template <class... Ps>
struct PolicyList {
    static constexpr std::size_t size = sizeof...(Ps);
};

namespace detail {

// Deduce an actor's `Policies...` from its `Actor<D, Ps...>` base. Declaration only (decltype/
// requires context) — never defined, never called at runtime.
template <class D, class... Ps>
PolicyList<Ps...> policies_from_base(const Actor<D, Ps...>&) noexcept;

// --- Per-element matchers for the value-carrying policies (partial-spec captures the arg). -------
template <class T>
struct as_priority {
    static constexpr bool present = false;
    static constexpr std::size_t value = 0;
};
template <std::size_t P>
struct as_priority<Priority<P>> {
    static constexpr bool present = true;
    static constexpr std::size_t value = P;
};

// Matches the forward-declared `DrainBudget<N>` by template-id and captures N WITHOUT needing the
// template to be complete (it is defined in engine.hpp) — pure pattern match.
template <class T>
struct as_budget {
    static constexpr bool present = false;
    static constexpr std::uint32_t value = 0;
};
template <std::uint32_t N>
struct as_budget<DrainBudget<N>> {
    static constexpr bool present = true;
    static constexpr std::uint32_t value = N;
};

template <class T>
struct as_maxconc {
    static constexpr bool present = false;
    static constexpr std::size_t value = 0;
};
template <std::size_t N>
struct as_maxconc<MaxConcurrency<N>> {
    static constexpr bool present = true;
    static constexpr std::size_t value = N;
};

// Matches `Persistent<EventSourced, Mode>` by template-id (persistence.hpp forward-declared above)
// — captures whether the actor declares the EVENT-SOURCED durability model, which the interim
// per-actor staging buffer (event_log.hpp) cannot serve safely under reentrancy.
template <class T>
struct as_event_sourced_persist {
    static constexpr bool present = false;
};
template <PersistMode Mode>
struct as_event_sourced_persist<::quark::Persistent<::quark::EventSourced, Mode>> {
    static constexpr bool present = true;
};

template <class T>
struct as_idle_ms {
    static constexpr bool present = false;
    static constexpr std::uint64_t value = 0;
};
template <std::uint64_t Ms>
struct as_idle_ms<IdleTimeout<Ms>> {
    static constexpr bool present = true;
    static constexpr std::uint64_t value = Ms;
};

// Matches `Delivery<Level>` by template-id and captures the declared level (017). Absent ⇒ the free
// default `AtMostOnce`, so the sum-fold below (identical to the Priority/idle folds) yields
// AtMostOnce (== 0) for a plain actor without instantiating any effectively-once machinery.
template <class T>
struct as_delivery {
    static constexpr bool present = false;
    static constexpr DeliveryLevel value = DeliveryLevel::AtMostOnce;
};
template <DeliveryLevel L>
struct as_delivery<::quark::Delivery<L>> {
    static constexpr bool present = true;
    static constexpr DeliveryLevel value = L;
};

// Matches ANY `Persistent<Model, Mode>` (either durability model) by template-id — persistence.hpp
// forward-declared the template above. Used by the 017 validation rule "EffectivelyOnce requires a
// Persistent actor" (independent of `as_event_sourced_persist`, which matches only EventSourced).
template <class T>
struct as_persistent {
    static constexpr bool present = false;
};
template <class Model, PersistMode Mode>
struct as_persistent<::quark::Persistent<Model, Mode>> {
    static constexpr bool present = true;
};

// --- Placement policy extraction (025 Part B). Pick the (at most one) `Placement<...>` from the pack;
//     default to `Placement<HashById>` when absent, so a plain actor's placement is the unchanged HRW.
template <class T>
struct as_placement {
    static constexpr bool present = false;
    using type = ::quark::Placement<::quark::HashById>;
};
template <class S, class... Ms>
struct as_placement<::quark::Placement<S, Ms...>> {
    static constexpr bool present = true;
    using type = ::quark::Placement<S, Ms...>;
};

template <class... Ps>
struct find_placement {
    using type = ::quark::Placement<::quark::HashById>;
};
template <class P, class... Rest>
struct find_placement<P, Rest...> {
    using type = std::conditional_t<as_placement<P>::present, typename as_placement<P>::type,
                                    typename find_placement<Rest...>::type>;
};

// True for a modifier that is an EMPTY `Require<>` (zero capabilities) — a config error (025).
template <class T>
struct is_empty_require : std::false_type {};
template <>
struct is_empty_require<::quark::Require<>> : std::true_type {};

// Structural facts about a resolved `Placement<S, Ms...>` used by validation (the runtime resolution
// lives in placement_policies.hpp; these are the COMPILE-TIME predicates 008/025 validate).
template <class P>
struct placement_info {
    using strategy = ::quark::HashById;
    static constexpr bool strategy_explicit = false;
    static constexpr bool has_empty_require = false;
    static constexpr bool has_weighted = false;
    static constexpr std::size_t modifier_count = 0;
};
template <class S, class... Ms>
struct placement_info<::quark::Placement<S, Ms...>> {
    using strategy = S;
    static constexpr bool strategy_explicit = std::is_same_v<S, ::quark::Explicit>;
    static constexpr bool has_empty_require = (is_empty_require<Ms>::value || ...);
    static constexpr bool has_weighted = (std::is_same_v<Ms, ::quark::Weighted> || ...);
    static constexpr std::size_t modifier_count = sizeof...(Ms);
};

// --- Stateless pool extraction (025 Part C). Match `Stateless<N, Opts...>`, capture N + `ClusterWide`.
template <class T>
struct as_stateless {
    static constexpr bool present = false;
    static constexpr std::size_t value = 0;
    static constexpr bool cluster_wide = false;
};
template <std::size_t N, class... Opts>
struct as_stateless<::quark::Stateless<N, Opts...>> {
    static constexpr bool present = true;
    static constexpr std::size_t value = N;
    static constexpr bool cluster_wide = (std::is_same_v<Opts, ::quark::ClusterWide> || ...);
};

// --- The traits, folded once over the recovered pack. -------------------------------------------
template <class L>
struct policy_traits;
template <class... Ps>
struct policy_traits<PolicyList<Ps...>> {
    static constexpr bool has_sequential = (std::is_same_v<Ps, ::quark::Sequential> || ...);
    static constexpr bool has_reentrant = (std::is_same_v<Ps, ::quark::Reentrant> || ...);
    static constexpr bool has_keepalive = (std::is_same_v<Ps, ::quark::KeepAlive> || ...);

    static constexpr std::size_t priority_count = (std::size_t{0} + ... + (as_priority<Ps>::present ? 1 : 0));
    static constexpr bool priority_present = priority_count != 0;
    static constexpr std::size_t priority =
        (std::size_t{0} + ... + (as_priority<Ps>::present ? as_priority<Ps>::value : std::size_t{0}));

    static constexpr std::size_t budget_count = (std::size_t{0} + ... + (as_budget<Ps>::present ? 1 : 0));
    static constexpr bool budget_present = budget_count != 0;
    static constexpr std::uint32_t budget =
        (std::uint32_t{0} + ... + (as_budget<Ps>::present ? as_budget<Ps>::value : std::uint32_t{0}));

    static constexpr std::size_t maxconc_count = (std::size_t{0} + ... + (as_maxconc<Ps>::present ? 1 : 0));
    static constexpr bool maxconc_present = maxconc_count != 0;
    static constexpr std::size_t maxconc =
        (std::size_t{0} + ... + (as_maxconc<Ps>::present ? as_maxconc<Ps>::value : std::size_t{0}));

    static constexpr bool idle_present = (as_idle_ms<Ps>::present || ...);
    static constexpr std::uint64_t idle_ms =
        (std::uint64_t{0} + ... + (as_idle_ms<Ps>::present ? as_idle_ms<Ps>::value : std::uint64_t{0}));

    static constexpr bool has_event_sourced_persist = (as_event_sourced_persist<Ps>::present || ...);

    // --- Delivery (017): the (at most one) Delivery level + whether the actor is persistent. --------
    static constexpr bool has_persistent = (as_persistent<Ps>::present || ...);
    static constexpr std::size_t delivery_count = (std::size_t{0} + ... + (as_delivery<Ps>::present ? 1 : 0));
    // Sum-fold the underlying value: AtMostOnce == 0 is the additive identity, so an actor with no
    // `Delivery<>` folds to AtMostOnce (the free default); with exactly one it folds to that level
    // (delivery_count <= 1 is a validation rule, so a second one — which would sum — is rejected).
    static constexpr DeliveryLevel delivery = static_cast<DeliveryLevel>(
        (unsigned{0} + ... +
         (as_delivery<Ps>::present ? static_cast<unsigned>(as_delivery<Ps>::value) : unsigned{0})));

    // --- Placement (025 Part B): the (at most one) Placement policy + its structural facts. --------
    static constexpr std::size_t placement_count = (std::size_t{0} + ... + (as_placement<Ps>::present ? 1 : 0));
    using placement = typename find_placement<Ps...>::type;
    using placement_facts = placement_info<placement>;

    // --- Stateless (025 Part C): pool presence, size N, cluster-wide opt-in. -----------------------
    static constexpr std::size_t stateless_count = (std::size_t{0} + ... + (as_stateless<Ps>::present ? 1 : 0));
    static constexpr bool has_stateless = stateless_count != 0;
    static constexpr std::size_t stateless_size =
        (std::size_t{0} + ... + (as_stateless<Ps>::present ? as_stateless<Ps>::value : std::size_t{0}));
    static constexpr bool stateless_cluster_wide = (as_stateless<Ps>::cluster_wide || ...);

    template <class Tag>
    static constexpr bool has = (std::is_same_v<Tag, Ps> || ...);
};

}  // namespace detail

// The recovered policy pack for actor `A` (compile error if `A` is not an `Actor<A, …>`).
template <class A>
using policies_of = decltype(detail::policies_from_base(std::declval<const A&>()));

// `A` is a well-formed Quark actor (derives from `Actor<A, …>`).
template <class A>
concept is_actor = requires(const A& a) { detail::policies_from_base(a); };

template <class A>
using policy_traits_of = detail::policy_traits<policies_of<A>>;

// --- The public trait extractors (005 §Metadata) — sensible defaults when a policy is absent. ---

// True iff `A` lists policy tag `Tag` (e.g. `has_policy_v<KeepAlive, A>`).
template <class Tag, class A>
inline constexpr bool has_policy_v = policy_traits_of<A>::template has<Tag>;

// Priority band (0 = highest). Default 0 when no `Priority<P>` is declared — matches the engine's
// `register_activation` band default; under `UniformFIFO` every band clamps to 0 regardless.
template <class A>
[[nodiscard]] consteval std::size_t priority_band_of() noexcept {
    return policy_traits_of<A>::priority;
}

// Per-actor drain budget. Default 0 ⇒ "use the engine-wide default" (the `register_activation`
// convention), so an un-annotated actor inherits `EngineConfig::drain_budget`.
template <class A>
[[nodiscard]] consteval std::uint32_t drain_budget_of() noexcept {
    return policy_traits_of<A>::budget;
}

// Declared reentrancy INTENT (015 seam). Distinct from the base's runtime `A::is_reentrant` flag
// (currently always false — 001/015): this reads what the AUTHOR declared via the policy pack.
// `Reentrant`, or `MaxConcurrency<N>` with N > 1, both express reentrant execution.
template <class A>
inline constexpr bool is_reentrant_v =
    policy_traits_of<A>::has_reentrant || (policy_traits_of<A>::maxconc > 1);

// Concurrency bound (015 seam). `MaxConcurrency<N>` → N; else `Reentrant` → 0 (== unbounded
// sentinel); else Sequential → 1 (one in-flight message).
template <class A>
[[nodiscard]] consteval std::size_t max_concurrency_of() noexcept {
    using T = policy_traits_of<A>;
    if (T::maxconc_present) return T::maxconc;
    return T::has_reentrant ? std::size_t{0} : std::size_t{1};
}

// Lifecycle: `KeepAlive` present, and the declared idle timeout in ms (0 ⇒ none / KeepAlive).
template <class A>
inline constexpr bool keeps_alive_v = policy_traits_of<A>::has_keepalive;
template <class A>
[[nodiscard]] consteval std::uint64_t idle_timeout_ms_of() noexcept {
    return policy_traits_of<A>::idle_ms;
}

// --- Delivery (017) — the resolved guarantee level + the persistence predicate. A plain actor with
// no `Delivery<>` resolves to `AtMostOnce` (the free default); `delivery.hpp` reads these to decide
// whether to instantiate the effectively-once machinery at all. ----------------------------------

// The actor's resolved delivery level; `AtMostOnce` when no `Delivery<>` is declared.
template <class A>
inline constexpr DeliveryLevel delivery_of = policy_traits_of<A>::delivery;

// The same as a callable extractor (mirrors `priority_band_of<A>()` etc.).
template <class A>
[[nodiscard]] consteval DeliveryLevel delivery_level_of() noexcept {
    return policy_traits_of<A>::delivery;
}

// True iff `A` declares any `Persistent<Model, Mode>` policy (either durability model).
template <class A>
inline constexpr bool is_persistent_v = policy_traits_of<A>::has_persistent;

// Validation CONDITION as a constexpr predicate (so tests assert the rule WITHOUT tripping the hard
// static_assert in validate_actor_policies): true iff `A` violates "EffectivelyOnce requires a
// Persistent actor" (017 §"The three levels"). `AtMostOnce`/`AtLeastOnce` never violate it.
template <class A>
[[nodiscard]] consteval bool effectively_once_needs_persistence() noexcept {
    return policy_traits_of<A>::delivery == DeliveryLevel::EffectivelyOnce &&
           !policy_traits_of<A>::has_persistent;
}

// --- Placement (025 Part B) — the resolved policy TYPE + structural predicates. The runtime resolver
// (placement_policies.hpp) consumes `placement_of<A>`; a plain actor gets `Placement<HashById>`. ------

// The actor's resolved placement policy (`Placement<Strategy, Modifiers...>`); default `Placement<HashById>`.
template <class A>
using placement_of = typename policy_traits_of<A>::placement;

// True iff `A` declares a stateless worker pool (025 Part C).
template <class A>
inline constexpr bool is_stateless_v = policy_traits_of<A>::has_stateless;

// The declared pool size N (0 if not stateless).
template <class A>
[[nodiscard]] consteval std::size_t stateless_size_of() noexcept {
    return policy_traits_of<A>::stateless_size;
}

// True iff the pool opts into cluster-wide routing (`Stateless<N, ClusterWide>`) — a 010/021 seam.
template <class A>
inline constexpr bool stateless_cluster_wide_v = policy_traits_of<A>::stateless_cluster_wide;

// True iff `A`'s placement strategy is `Explicit`.
template <class A>
inline constexpr bool placement_is_explicit_v = policy_traits_of<A>::placement_facts::strategy_explicit;

// True iff `A`'s placement uses the `Weighted` (proportional log-WRH, ADR-013) modifier.
template <class A>
inline constexpr bool placement_is_weighted_v = policy_traits_of<A>::placement_facts::has_weighted;

// --- Validation CONDITIONS as constexpr predicates (so tests can assert the rules WITHOUT tripping the
// hard static_asserts in validate_actor_policies). Each is `true` iff the corresponding rule is VIOLATED.
template <class A>
[[nodiscard]] consteval bool stateless_explicit_conflict() noexcept {
    return is_stateless_v<A> && placement_is_explicit_v<A>;
}
template <class A>
[[nodiscard]] consteval bool stateless_persistence_conflict() noexcept {
    return is_stateless_v<A> && policy_traits_of<A>::has_event_sourced_persist;
}
template <class A>
[[nodiscard]] consteval bool empty_require_present() noexcept {
    return policy_traits_of<A>::placement_facts::has_empty_require;
}

// ============================================================================================
// Validation (005 §Validation, fail-fast — the compile-time subset). Returns true; the teeth are
// the internal `static_assert`s, which fire when this is instantiated (register_actor calls it).
// ============================================================================================
template <class A>
consteval bool validate_actor_policies() noexcept {
    using T = policy_traits_of<A>;

    // Conflicting execution policies (005): Sequential is single-executor; Reentrant / a >1
    // concurrency cap contradict it.
    static_assert(!(T::has_sequential && T::has_reentrant),
                  "conflicting execution policies: Sequential + Reentrant (005 §Validation)");
    static_assert(!(T::has_sequential && T::maxconc_present && T::maxconc > 1),
                  "conflicting execution policies: Sequential + MaxConcurrency<N>, N>1 (005 §Validation)");

    // Persistent<EventSourced> is Sequential-ONLY in this interim (mirrors the Sequential-only
    // Transactional<> rule, ADR-009). The EventLog staging buffer is per-ACTOR, not per-in-flight-
    // handler, so a reentrant actor's interleaved handlers would corrupt each other's staged events
    // (B's commit flushes A's staged batch; A's rollback becomes a no-op → ADR-009 C7 violated).
    // Forbid it at COMPILE TIME rather than allow silent corruption. Proper Reentrant EventSourced
    // (per-in-flight-handler staging with commit-time seq allocation) is DEFERRED — see the note on
    // `EventLog::staged_` in event_log.hpp.
    static_assert(!(T::has_event_sourced_persist && (T::has_reentrant ||
                                                     (T::maxconc_present && T::maxconc > 1))),
                  "Persistent<EventSourced> is Sequential-only in this interim: per-actor event "
                  "staging is unsafe under Reentrant / MaxConcurrency<N>; per-handler staging is "
                  "DEFERRED (012/ADR-009 C7). Use Sequential, or Persistent<Snapshot>.");

    // Lifecycle: KeepAlive and IdleTimeout are mutually exclusive knobs (013 — KeepAlive is the
    // idle_ticks==0 sentinel; an explicit timeout contradicts it).
    static_assert(!(T::has_keepalive && T::idle_present),
                  "conflicting lifecycle policies: KeepAlive + IdleTimeout<Ms> (005 §Validation)");

    // At most one of each value-carrying scheduling policy (a second one would silently sum).
    static_assert(T::priority_count <= 1, "at most one Priority<P> per actor (005 §Validation)");
    static_assert(T::budget_count <= 1, "at most one DrainBudget<N> per actor (005 §Validation)");
    static_assert(T::maxconc_count <= 1, "at most one MaxConcurrency<N> per actor (005 §Validation)");

    // --- Placement + Stateless (025 §Validation) --------------------------------------------------
    static_assert(T::placement_count <= 1, "at most one Placement<...> per actor (025 §Validation)");
    static_assert(T::stateless_count <= 1, "at most one Stateless<N> per actor (025 §Validation)");

    // A Stateless pool has NO ActorId→node pin, so a caller-specified `Explicit` node is meaningless.
    static_assert(!stateless_explicit_conflict<A>(),
                  "conflicting policies: Stateless + Placement<Explicit> — a stateless pool has no "
                  "identity to pin to an explicit node (025 §Validation / §Interaction 005)");

    // A Stateless actor is non-durable by construction (no cross-message state); persistence /
    // effectively-once (012/017) do not apply and are rejected rather than silently ignored.
    static_assert(!stateless_persistence_conflict<A>(),
                  "conflicting policies: Stateless + Persistent<EventSourced> — a stateless actor "
                  "holds no durable state (025 §Interaction 012/017)");

    // An empty `Require<>` (no capabilities) narrows nothing — always a config mistake (025).
    static_assert(!empty_require_present<A>(),
                  "empty Require<> constraint: a Require with no capabilities narrows nothing "
                  "(025 §Validation — config-time error)");

    // --- Delivery guarantees (017 §"The three levels" / §Validation) ------------------------------
    static_assert(T::delivery_count <= 1, "at most one Delivery<Level> per actor (017 §Validation)");

    // EffectivelyOnce needs durable dedup state (the per-sender watermark + transactional outbox) and
    // a fencing token, both of which require a Persistent actor over a linearizable (CP) StateStore
    // (017 §"The three levels" / §"The consistency price"). Reject it at COMPILE TIME on a
    // non-persistent actor. AtMostOnce (the default) requires nothing; AtLeastOnce requires only an
    // idempotent handler (a documented runtime contract, not compile-checkable). The store's
    // linearizability is a RUNTIME property of the chosen adapter, checked in delivery.hpp.
    static_assert(!effectively_once_needs_persistence<A>(),
                  "Delivery<EffectivelyOnce> requires a Persistent<...> actor (+ a linearizable "
                  "StateStore): effectively-once effects need fenced durable dedup state (017 "
                  "§\"The three levels\" / §\"The consistency price\").");
    return true;
}

}  // namespace quark
