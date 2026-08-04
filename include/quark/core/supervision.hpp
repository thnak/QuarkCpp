// Implements 007-Failure-and-Supervision §Supervision decision + ADR-009 — the COMPILE-TIME
// supervision policy catalog and the `supervision_of<A>()` extractor that lowers an actor's
// `OnFailure<Decision, MaxRestarts<N, Within<…>>>` policy pack to the runtime `SupervisionPolicy`
// the (type-erased) Activation consumes. Same shape as 005 `policies.hpp` / `max_concurrency_of<A>`:
// pure `if constexpr` / pack-fold, no RTTI, no reflection, no virtual — the engine resolves it once
// at registration and hands the runtime image into the Activation ctor.
//
// The RUNTIME image (`SupervisionPolicy`, `SupervisionDirective`, the sinks) lives in `activation.hpp`
// so the execution core carries no compile-time policy dependency; this header is the bridge.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "quark/core/activation.hpp"  // SupervisionPolicy + SupervisionDirective (runtime image)
#include "quark/core/policies.hpp"    // policies_of<A> pack recovery (no actor.hpp edit)

namespace quark {

// ============================================================================================
// Supervision policy catalog (007 §Supervision decision). Each directive is a TAG type; each budget
// knob is a value-carrying template. Declared on the actor:
//
//   class Order : public quark::Actor<Order, Sequential,
//                     OnFailure<Restart, MaxRestarts<3, Within<10'000>>>> {};
//
// Absent `OnFailure<…>` ⇒ the spec default: Restart with an (effectively) unbounded budget.
// ============================================================================================

// The four directives (007). Tag types so they compose in the policy pack by being LISTED.
struct Resume {};
struct Restart {};
struct Stop {};
struct Escalate {};

// `Within<Millis>` — the MaxRestarts sliding window, in MILLISECONDS. (The spec sketch writes it as
// a chrono duration `Within<seconds<10>>`; a millisecond NTTP avoids literal-class-NTTP complexity
// and reads the same at the call site — `Within<10'000>` == 10 s. Reported deviation.)
template <std::int64_t Millis>
struct Within {
    static constexpr std::int64_t ms = Millis;
    static constexpr std::int64_t ns = Millis * 1'000'000;
};

// `MaxRestarts<N, Within<…>>` — bound N restarts per window. Window defaults to 0 (no window: the
// count never resets — a hard lifetime cap).
template <std::size_t N, class Window = Within<0>>
struct MaxRestarts {
    static constexpr std::uint32_t value = static_cast<std::uint32_t>(N);
    static constexpr std::int64_t window_ns = Window::ns;
};

// `OnFailure<Decision, Budget...>` — the actor's failure policy. `Decision` is one of the four tags;
// `Budget` is an optional `MaxRestarts<…>`.
template <class Decision, class... Budget>
struct OnFailure {};

// --- Adopted D3 knobs (007 §ask reply on Restart / §resource failure) — WIRED (ADR-009 post-
// decision implementation note; see the extractor `supervision_of<A>()` below, which folds these
// into `SupervisionPolicy::max_ask_retries`, and `Activation::handle_restart_retry`). ------------
// `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`: default `Fail` (the poison message is dead-
// lettered before Restart runs; the runtime delivers this via the Responder-on-reclaim path).
// `Retry<N,…>` is opt-in, Sequential + sync-handler only (`validate_supervision_policies<A>()`).
struct Fail {};
template <class Key>
struct IdempotencyKey {
    using key = Key;
};
template <std::size_t N, class Key>
struct Retry {
    static constexpr std::size_t value = N;
    using key = Key;
};
template <class Mode = Fail>
struct OnRestartAsk {
    using mode = Mode;
};

// `OnResourceFailure<FailMessage | Degrade>` (004/007): default FailMessage (a failed PerMessage<T>
// factory fails the message via the same boundary). `Degrade` is the explicit opt-in.
struct FailMessage {};
struct Degrade {};
template <class Mode = FailMessage>
struct OnResourceFailure {
    using mode = Mode;
};

// `Supervision<Node | PerType | Tree<…>>` (007 §Escalation) — the escalation topology knob. `Node`
// (default) is a depth-1 tree: a single node supervisor. WIRED for eager `spawn<A>()` (see
// `Engine::wire_escalation<A>`, `engine.hpp`) — `Tree<S0,…>` routes a single hop to `S0` only (no
// chain-forwarding); `declare_lazy<A>()` is a documented residual. The runtime storm guards
// (`EscalationGuard` below: TTL, aggregate + per-source rate limiting) ARE wired — see
// `Engine::set_node_supervisor`/`set_supervisor`/`set_type_supervisor`. The static depth bound is
// enforced here (compile time); the topology EXTRACTION (`supervision_topology_of<A>`, below) is a
// pure function of `A`'s policy pack, consumed by `engine.hpp`'s routing.
struct Node {};
struct PerType {};
template <class... Supervisors>
struct Tree {
    static constexpr std::size_t depth = sizeof...(Supervisors);
    static_assert(depth <= 8, "supervision Tree depth must be ≤ 8 (007 §Escalation, consteval bound)");
};
template <class Topology = Node>
struct Supervision {
    using topology = Topology;
};

namespace detail {

// Map a directive TAG to the runtime enum.
template <class D>
struct directive_of;
template <>
struct directive_of<Resume> {
    static constexpr SupervisionDirective value = SupervisionDirective::Resume;
};
template <>
struct directive_of<Restart> {
    static constexpr SupervisionDirective value = SupervisionDirective::Restart;
};
template <>
struct directive_of<Stop> {
    static constexpr SupervisionDirective value = SupervisionDirective::Stop;
};
template <>
struct directive_of<Escalate> {
    static constexpr SupervisionDirective value = SupervisionDirective::Escalate;
};

// Per-element matcher: capture the `OnFailure<Decision, Budget...>` in the pack (if present).
template <class T>
struct as_on_failure {
    static constexpr bool present = false;
    static constexpr SupervisionDirective decision = SupervisionDirective::Restart;
    static constexpr std::uint32_t max_restarts = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::int64_t window_ns = 0;
};
template <class Decision>
struct as_on_failure<OnFailure<Decision>> {
    static constexpr bool present = true;
    static constexpr SupervisionDirective decision = directive_of<Decision>::value;
    static constexpr std::uint32_t max_restarts = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::int64_t window_ns = 0;
};
template <class Decision, std::size_t N, class Window>
struct as_on_failure<OnFailure<Decision, MaxRestarts<N, Window>>> {
    static constexpr bool present = true;
    static constexpr SupervisionDirective decision = directive_of<Decision>::value;
    static constexpr std::uint32_t max_restarts = MaxRestarts<N, Window>::value;
    static constexpr std::int64_t window_ns = MaxRestarts<N, Window>::window_ns;
};

// Per-element matcher: capture the (at most one) `OnRestartAsk<Mode>` in the pack (007 §"ask reply
// on Restart"). `Fail` (the default when absent) folds to `retry == false`.
template <class T>
struct as_on_restart_ask {
    static constexpr bool present = false;
    static constexpr bool retry = false;
    static constexpr std::uint32_t retry_count = 0;
};
template <>
struct as_on_restart_ask<OnRestartAsk<Fail>> {
    static constexpr bool present = true;
    static constexpr bool retry = false;
    static constexpr std::uint32_t retry_count = 0;
};
template <std::size_t N, class Key>
struct as_on_restart_ask<OnRestartAsk<Retry<N, Key>>> {
    static constexpr bool present = true;
    static constexpr bool retry = true;
    static constexpr std::uint32_t retry_count = static_cast<std::uint32_t>(N);
};

// Fold the recovered pack: find the (at most one) OnFailure/OnRestartAsk and lower them to
// SupervisionPolicy.
template <class L>
struct supervision_traits;
template <class... Ps>
struct supervision_traits<PolicyList<Ps...>> {
    static constexpr std::size_t count =
        (std::size_t{0} + ... + (as_on_failure<Ps>::present ? 1 : 0));
    static_assert(count <= 1, "at most one OnFailure<…> per actor (007 §Supervision decision)");

    static constexpr bool present = count != 0;
    // At most one element is present, so an integer OR-fold (0 identity) selects its value. Enum
    // folds go through the underlying type (enum class has no operator|); guard the absent case.
    static constexpr std::uint8_t decision_raw =
        (std::uint8_t{0} | ... |
         (as_on_failure<Ps>::present ? static_cast<std::uint8_t>(as_on_failure<Ps>::decision)
                                     : std::uint8_t{0}));
    static constexpr SupervisionDirective decision =
        present ? static_cast<SupervisionDirective>(decision_raw) : SupervisionDirective::Restart;
    static constexpr std::uint32_t max_restarts =
        present ? (std::uint32_t{0} | ... |
                   (as_on_failure<Ps>::present ? as_on_failure<Ps>::max_restarts : std::uint32_t{0}))
                : std::numeric_limits<std::uint32_t>::max();
    static constexpr std::int64_t window_ns =
        (std::int64_t{0} + ... +
         (as_on_failure<Ps>::present ? as_on_failure<Ps>::window_ns : std::int64_t{0}));

    static constexpr std::size_t retry_ask_count =
        (std::size_t{0} + ... + (as_on_restart_ask<Ps>::present ? 1 : 0));
    static_assert(retry_ask_count <= 1,
                  "at most one OnRestartAsk<…> per actor (007 §\"ask reply on Restart\")");
    static constexpr bool retry_ask = (false || ... || as_on_restart_ask<Ps>::retry);
    static constexpr std::uint32_t max_ask_retries =
        (std::uint32_t{0} + ... +
         (as_on_restart_ask<Ps>::retry ? as_on_restart_ask<Ps>::retry_count : std::uint32_t{0}));
};

}  // namespace detail

// The public extractor: the resolved runtime `SupervisionPolicy` for actor `A` (the spec default
// when no `OnFailure<…>` is declared). The engine calls this at registration and passes the result
// into the Activation ctor — exactly like `max_concurrency_of<A>()`.
template <class A>
[[nodiscard]] consteval SupervisionPolicy supervision_of() noexcept {
    using T = detail::supervision_traits<policies_of<A>>;
    return SupervisionPolicy{T::decision, T::max_restarts, T::window_ns,
                             T::retry_ask ? T::max_ask_retries : std::uint32_t{0}};
}

// `OnRestartAsk<Retry<N,IdempotencyKey>>` is Sequential-only for now (Activation::
// handle_restart_retry re-dispatches the SAME faulting message against the freshly-reconstructed
// actor inline on the fault path, proven only for exactly one in-flight message — mirrors the
// existing `Transactional<>`/`IdleTimeout<Ms>` Sequential-only restrictions, policies.hpp). Called
// alongside `validate_actor_policies<A>()` at every registration site (spawn.hpp, engine.hpp,
// metadata.hpp).
template <class A>
consteval bool validate_supervision_policies() noexcept {
    using T = detail::supervision_traits<policies_of<A>>;
    static_assert(!(T::retry_ask && is_reentrant_v<A>),
                  "OnRestartAsk<Retry<N,IdempotencyKey>> is Sequential-only for now: the retry loop "
                  "re-dispatches the SAME faulting message against the freshly-reconstructed actor "
                  "inline on the fault path, proven only for exactly one in-flight message (mirrors "
                  "Transactional<>/IdleTimeout<Ms>'s existing Sequential-only restriction, "
                  "policies.hpp). Reentrant/MaxConcurrency<N> support is deferred.");
    return true;
}

namespace detail {

// Per-element matcher: capture the (at most one) `OnResourceFailure<Mode>` in the pack.
template <class T>
struct as_on_resource_failure {
    static constexpr bool present = false;
    static constexpr bool degrade = false;
};
template <>
struct as_on_resource_failure<OnResourceFailure<FailMessage>> {
    static constexpr bool present = true;
    static constexpr bool degrade = false;
};
template <>
struct as_on_resource_failure<OnResourceFailure<Degrade>> {
    static constexpr bool present = true;
    static constexpr bool degrade = true;
};

template <class L>
struct resource_failure_traits;
template <class... Ps>
struct resource_failure_traits<PolicyList<Ps...>> {
    static constexpr std::size_t count =
        (std::size_t{0} + ... + (as_on_resource_failure<Ps>::present ? 1 : 0));
    static_assert(count <= 1, "at most one OnResourceFailure<…> per actor (004/007 §Validation)");
    // Absent ⇒ FailMessage (the free default, 004/007 C4).
    static constexpr bool degrade =
        (false || ... || (as_on_resource_failure<Ps>::present && as_on_resource_failure<Ps>::degrade));
};

}  // namespace detail

// True iff `A` declares `OnResourceFailure<Degrade>`; false (FailMessage, the default) otherwise. A
// handler branches on this to decide whether a failed `PerMessage<T>::acquire()` should proceed
// degraded (Degrade) or call `ProductGuard::acquire_or_throw()` to fail the message (FailMessage).
template <class A>
[[nodiscard]] consteval bool resource_failure_degrades() noexcept {
    return detail::resource_failure_traits<policies_of<A>>::degrade;
}

// ============================================================================================
// 007 §Escalation (ADR-009 `Supervision<Node|PerType|Tree<…>>`) — the standard escalation message a
// supervisor actor handles, plus the compile-time TOPOLOGY extraction the engine (engine.hpp) uses to
// route a fault. Routing itself (the type-erased `EscalationRouteFn`, the supervisor registries, and
// `Engine::route_escalation`) is an engine.hpp concern — this header only exposes what the actor
// DECLARED, exactly like `placement_of<A>` (policies.hpp) exposes a declared `Placement<…>` without
// itself knowing how to resolve one.
// ============================================================================================

// The message a `Supervision<…>` target must `handle(const Escalated&)`. `source` is the ActorId of
// the actor whose fault escalated; `cause` is the fault's own recorded error (007 §Per-message
// outcome — the SAME error the dead-letter sink observed).
struct Escalated {
    ActorId source{};
    error cause{};
};

// ============================================================================================
// Escalation-storm guards (007 §Escalation residual — see ADR-009's post-decision note). A
// supervisor is an ordinary actor with an ordinary mailbox: with no bound, a systemic fault (many
// actors escalating at once) or a respawn→refault loop (a supervisor that restarts its escalated
// child, which immediately faults again) can flood it at native `tell` throughput. `EscalationGuard`
// is a RUNTIME parameter to `Engine::set_node_supervisor`/`set_supervisor`/`set_type_supervisor` —
// not a compile-time actor policy — because it describes the tolerance of one specific supervisor
// REGISTRATION, not a trait of the escalating actor's type. All-zero (the default) is unbounded:
// byte-for-byte the pre-existing behavior, so every existing call site is unaffected.
//
//   * `ttl_ns` — reuses the EXISTING 018/022 deadline-aware-shedding path (`activation.hpp`'s
//     drain loop already sheds a doomed, past-deadline message under overload): the escalation
//     routing stamps the posted `Escalated` descriptor's `deadline_ns` at `now + ttl_ns` instead of
//     the un-set `0`. A supervisor actor also configured with `ShedThreshold<N>` (022) then sheds a
//     stale escalation instead of acting on it once it finally reaches the front of a backed-up
//     mailbox. 0 = never expires (today's behavior).
//   * `max_per_sec` / `burst` — an aggregate token-bucket (022 `TokenBucket`) admission gate over
//     ALL escalations reaching this supervisor, regardless of source. 0 = unbounded.
//   * `max_per_source` / `per_source_window_ns` — a sliding-window cap (the SAME shape as the
//     per-actor `MaxRestarts<N,Within<…>>` budget above, applied per ESCALATING actor id instead of
//     per restarting actor) bounding how many escalations from the SAME faulting actor this
//     supervisor accepts per window — the direct defense against a respawn→refault loop. 0 =
//     unbounded.
//
// A shed escalation is never posted (no pool acquire, no mailbox touch) and is counted exactly via
// `Engine::escalations_shed()` — never a silent drop.
struct EscalationGuard {
    std::int64_t ttl_ns = 0;
    double max_per_sec = 0.0;
    double burst = 0.0;
    std::uint32_t max_per_source = 0;
    std::int64_t per_source_window_ns = 0;
};

namespace detail {

// Per-element matcher: capture the (at most one) `Supervision<Topology>` in the pack; default `Node`.
template <class T>
struct as_supervision {
    static constexpr bool present = false;
    using topology = Node;
};
template <class Topology>
struct as_supervision<Supervision<Topology>> {
    static constexpr bool present = true;
    using topology = Topology;
};

template <class... Ps>
struct find_supervision {
    using type = Node;
};
template <class P, class... Rest>
struct find_supervision<P, Rest...> {
    using type = std::conditional_t<as_supervision<P>::present, typename as_supervision<P>::topology,
                                    typename find_supervision<Rest...>::type>;
};

template <class L>
struct supervision_topology_traits;
template <class... Ps>
struct supervision_topology_traits<PolicyList<Ps...>> {
    static constexpr std::size_t count = (std::size_t{0} + ... + (as_supervision<Ps>::present ? 1 : 0));
    static_assert(count <= 1, "at most one Supervision<…> per actor (007 §Escalation)");
    using topology = typename find_supervision<Ps...>::type;
};

// Structural predicates over a resolved topology TYPE (not the actor pack) — mirrors
// `placement_info<P>` (policies.hpp).
template <class T>
struct is_tree_topology : std::false_type {};
template <class... Ss>
struct is_tree_topology<Tree<Ss...>> : std::true_type {};

template <class T>
struct tree_first_supervisor;
template <class S0, class... Rest>
struct tree_first_supervisor<Tree<S0, Rest...>> {
    using type = S0;
};

}  // namespace detail

// The actor's resolved escalation topology (`Node` (default) | `PerType` | `Tree<Supervisors...>`).
template <class A>
using supervision_topology_of = typename detail::supervision_topology_traits<policies_of<A>>::topology;

template <class Topology>
inline constexpr bool is_tree_topology_v = detail::is_tree_topology<Topology>::value;
template <class Topology>
inline constexpr bool is_per_type_topology_v = std::is_same_v<Topology, PerType>;

// The FIRST supervisor type named by a `Tree<S0, S1, …>` topology (this pass routes a single hop to
// S0; multi-hop chain-forwarding through S1.. is a documented residual — see ADR-009/README).
template <class Topology>
using tree_first_supervisor_t = typename detail::tree_first_supervisor<Topology>::type;

}  // namespace quark
