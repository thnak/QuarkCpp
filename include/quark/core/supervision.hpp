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

// --- Adopted D3 knobs (007 §ask reply on Restart / §resource failure) — declared surface. --------
// `OnRestartAsk<Fail | Retry<N, IdempotencyKey>>`: default `Fail` (the poison ask resolves to a
// failure value; the runtime already delivers this via the Responder-on-reclaim path). `Retry<N,…>`
// is opt-in and STATIC-ASSERTs an idempotency fence (ADR-009 residual risk #6 — proven in D3's
// harness, integration against D1's guard is a gated seam; declared here, not yet auto-wired).
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
// (default) is a depth-1 tree: a single node supervisor. Runtime topology is a 021/026 seam; the
// static depth bound is enforced here.
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

// Fold the recovered pack: find the (at most one) OnFailure and lower it to SupervisionPolicy.
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
};

}  // namespace detail

// The public extractor: the resolved runtime `SupervisionPolicy` for actor `A` (the spec default
// when no `OnFailure<…>` is declared). The engine calls this at registration and passes the result
// into the Activation ctor — exactly like `max_concurrency_of<A>()`.
template <class A>
[[nodiscard]] consteval SupervisionPolicy supervision_of() noexcept {
    using T = detail::supervision_traits<policies_of<A>>;
    return SupervisionPolicy{T::decision, T::max_restarts, T::window_ns};
}

}  // namespace quark
