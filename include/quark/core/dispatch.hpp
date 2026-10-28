// Implements ADR-007 §Dispatch — the dense per-actor jump table. An actor declares
// `using protocol = quark::Protocol<M1, M2, ...>`; the send site stamps a compile-time dense
// slot into the descriptor; the drain does ONE `.rodata` function-pointer indexed indirect call
// `thunks[slot](self, desc, ctx)` — no RTTI, no reflection, no virtual, no per-message branch.
//
// The slot is computed by `slot_of<A,M>()` (consteval), guarded by `Handles<A,M> :=
// InProtocol<A,M> && has_handler<A,M>` (ADR-007's proven fix — both "unhandled" and
// "handled-but-unlisted" are compile errors; `slot == size` is structurally unreachable).
// Sync vs async is selected at compile time by the handler's return type (void vs quark::task<>).
#pragma once

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "quark/core/config.hpp"
#include "quark/core/descriptor.hpp"
#include "quark/core/message_context.hpp"
#include "quark/core/task.hpp"

namespace quark {

// --- Protocol: the manual message enumeration (ADR-007 residual-risk: std C++23 cannot walk a
// member overload set, so the list is explicit; a QUARK_PROTOCOL macro + lint is the drift
// mitigation, deferred). Membership and the dense slot are compile-time constants. ------------
template <class... Ms>
struct Protocol {
    static constexpr std::size_t size = sizeof...(Ms);

    template <class M>
    static constexpr bool contains = (std::is_same_v<M, Ms> || ...);

    // Dense index of M in the protocol, [0, size). Compile error if M is not listed.
    template <class M>
    [[nodiscard]] static consteval std::size_t index_of() {
        static_assert((std::is_same_v<M, Ms> || ...), "message type is not in this Protocol");
        std::size_t idx = size;
        std::size_t i = 0;
        (((std::is_same_v<M, Ms> && idx == size) ? (idx = i) : idx, ++i), ...);
        return idx;
    }
};

// --- Handler detection (concepts). Supports the spec's `handle(const M&)` plus an optional
// `handle(const M&, const MessageContext&)` overload so the ambient context is available to a
// handler without hidden mutable actor state. Sync ⇒ returns void; async ⇒ returns quark::task<>.
template <class A, class M>
concept handles_with_ctx =
    requires(A& a, const M& m, const MessageContext& c) { a.handle(m, c); };
template <class A, class M>
concept handles_plain = requires(A& a, const M& m) { a.handle(m); };
template <class A, class M>
concept has_handler = handles_with_ctx<A, M> || handles_plain<A, M>;

template <class A, class M>
QUARK_ALWAYS_INLINE decltype(auto) invoke_handle(A& a, const M& m, const MessageContext& c) {
    if constexpr (handles_with_ctx<A, M>)
        return a.handle(m, c);
    else
        return a.handle(m);
}

template <class A, class M>
using handle_result_t = decltype(invoke_handle(std::declval<A&>(), std::declval<const M&>(),
                                               std::declval<const MessageContext&>()));

template <class A, class M>
concept sync_handler = has_handler<A, M> && std::is_void_v<handle_result_t<A, M>>;
template <class A, class M>
concept async_handler = has_handler<A, M> && std::is_same_v<handle_result_t<A, M>, task<>>;

// Protocol membership (structural) AND handler existence — the ADR-007 proven guard.
template <class A, class M>
concept InProtocol = A::protocol::template contains<M>;
template <class A, class M>
concept Handles = InProtocol<A, M> && has_handler<A, M>;

// --- Dispatch result + the type-erased thunk / table --------------------------------------
enum class HandlerKind : std::uint8_t { Sync, Async };

struct DispatchOutcome {
    HandlerKind kind = HandlerKind::Sync;
    std::coroutine_handle<> frame{};  // valid iff kind == Async (may already be .done())
};

// One indexed indirect call target. NOT `noexcept` (007/ADR-009): a throwing SYNC handler — and a
// coroutine-frame allocation failure while starting an ASYNC handler — must PROPAGATE out of the
// thunk to the handler-boundary guard in `Activation::drain_step`, which catches it (Itanium
// zero-cost on the no-throw path — nothing added to the success path, ADR-009 F1/F2), fails any
// pending `ask` reply, and applies the supervision decision. An async handler's own body throw is
// captured by its coroutine promise (`task<>::unhandled_exception`) and surfaced via
// `async_frame_faulted`; it does not propagate here. See task.hpp / activation.hpp.
using Thunk = DispatchOutcome (*)(void* self, Descriptor* d, const MessageContext& ctx);

// A plain function-pointer array (0 vtable, .rodata) + its size — carried by value (ptr + size).
struct DispatchTable {
    const Thunk* thunks = nullptr;
    std::size_t size = 0;
};

// The per-(A,M) thunk: static_cast the erased self/payload (type-sound, no RTTI — ADR-007 S2),
// select sync vs async at compile time, and for async START the frame and report suspend-vs-done.
template <class A, class M>
DispatchOutcome dispatch_thunk(void* self, Descriptor* d, const MessageContext& ctx) {
    A& actor = *static_cast<A*>(self);
    const M& msg = *static_cast<const M*>(d->payload);
    if constexpr (sync_handler<A, M>) {
        invoke_handle(actor, msg, ctx);  // run to completion inline on the worker lane
        return {HandlerKind::Sync, {}};
    } else {
        static_assert(async_handler<A, M>, "handle() must return void (sync) or quark::task<> (async)");
        task<> t = invoke_handle(actor, msg, ctx);
        std::coroutine_handle<> h = t.detach();  // 015 seam: executor now owns the frame
        h.resume();                              // start; runs to first co_await or to final_suspend
        return {HandlerKind::Async, h};
    }
}

// Build the actor's `.rodata` thunk table from its protocol type list.
template <class A, class Proto>
struct thunk_table_of;
template <class A, class... Ms>
struct thunk_table_of<A, Protocol<Ms...>> {
    static constexpr std::array<Thunk, sizeof...(Ms)> value{&dispatch_thunk<A, Ms>...};
};

// --- Send-site slot stamping (ADR-007) ----------------------------------------------------
template <class A, class M>
[[nodiscard]] consteval std::uint16_t slot_of() {
    static_assert(InProtocol<A, M>, "message type is not listed in the actor's protocol (unlisted)");
    static_assert(has_handler<A, M>, "actor declares no handle() for this message type");
    return static_cast<std::uint16_t>(A::protocol::template index_of<M>());
}

// The dense msg-slot rides Descriptor::reserved (the 003 'future use' u16). 001 must not modify
// descriptor.hpp; 002/008 formalize a named `msg_slot_` field. The slot is process-local and must
// never be serialized or forwarded across actor types (ADR-007 residual risk).
QUARK_ALWAYS_INLINE void stamp_slot(Descriptor& d, std::uint16_t slot) noexcept { d.reserved = slot; }
[[nodiscard]] QUARK_ALWAYS_INLINE std::uint16_t slot_from(const Descriptor& d) noexcept {
    return d.reserved;
}
template <class A, class M>
QUARK_ALWAYS_INLINE void stamp(Descriptor& d) noexcept {
    d.reserved = slot_of<A, M>();
}

}  // namespace quark
