// Implements 001-Actor-Execution-Model §Hybrid handler execution + ADR-007 §authoring API — the
// CRTP actor base. `class Order : public Actor<Order, Sequential>` declares its protocol and its
// `handle` overloads; the base materializes the `.rodata` dispatch table (one indexed indirect
// call, no virtual/RTTI on the hot path). `Sequential` is the default policy (single-executor,
// reentrancy disabled — 001 §Reentrancy); `Reentrant`/`MaxConcurrency<N>` are 015 seams.
#pragma once

#include <utility>

#include "quark/core/dispatch.hpp"

namespace quark {

// Default execution policy (001): single-executor, non-reentrant, replies in request order.
struct Sequential {};

template <class Derived, class... Policies>
class Actor {
public:
    // The dense-slot dispatch table for this actor type — a plain function-pointer array in
    // `.rodata`, built from `Derived::protocol`. Returned BY VALUE (ptr + size, trivially copied);
    // the Activation stores it by value so there is no dangling-reference footgun.
    [[nodiscard]] static DispatchTable dispatch_table() noexcept {
        using Table = thunk_table_of<Derived, typename Derived::protocol>;
        return DispatchTable{Table::value.data(), Table::value.size()};
    }

    // 015 SEAM: Sequential is non-reentrant. Reentrant/MaxConcurrency<N> flip this and change
    // execution semantics only (enqueue/ordering/ownership unchanged) — not implemented in 001.
    static constexpr bool is_reentrant = false;

    // Deducing-this convenience: the most-derived reference (no virtual dispatch).
    template <class Self>
    [[nodiscard]] auto&& derived(this Self&& self) noexcept {
        return std::forward<Self>(self);
    }

protected:
    Actor() = default;
    Actor(const Actor&) = default;
    Actor(Actor&&) = default;
    Actor& operator=(const Actor&) = default;
    Actor& operator=(Actor&&) = default;
    ~Actor() = default;
};

}  // namespace quark
