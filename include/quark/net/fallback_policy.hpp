// Implements 010-Distribution §"Direct↔relay endpoint fallback (ADR-048)" — the compile-time policy
// tags TcpTransport's relay-fallback threshold resolves to a plain runtime `int64_t` at construction,
// matching the `DrainBudget<N>`-style "tag in, resolved runtime field out, zero cost when unused" idiom
// already used elsewhere in `policies.hpp`. TcpTransport itself is NOT templated on this policy — see
// ADR-048's Decision section for why Design 2 (this one) was chosen over the templated alternative.
//
// `NoRelayFallback` (the default) leaves TcpTransport byte-identical to its pre-ADR-048 behavior: no
// give-up timer is ever armed, and a `set_relay()` call is inert (there is nothing to fall back to).
// `DirectThenRelay<TimeoutMs>` gives a peer's first-ever `St::Connecting` attempt `TimeoutMs` wall-clock
// milliseconds to reach `St::Open` before TcpTransport gives up and redials that peer through its
// configured relay `Endpoint` (`TcpTransport::set_relay`); the same constant also paces the out-of-band
// probe that later recovers the peer back to a direct connection — a disclosed residual (ADR-048): one
// constant, double duty, no separate backoff/jitter of its own on the probe cadence.
#pragma once

#include <cstdint>

namespace quark::net {

// Resolved value 0 means "fallback disabled" — TcpTransport's original forever-retry-direct behavior.
struct NoRelayFallback {
    static constexpr std::int64_t timeout_ns = 0;
};

template <std::int64_t TimeoutMs>
struct DirectThenRelay {
    static_assert(TimeoutMs > 0,
                  "DirectThenRelay<TimeoutMs> needs a positive give-up threshold; use NoRelayFallback "
                  "to disable relay fallback instead of DirectThenRelay<0>");
    static constexpr std::int64_t timeout_ns = TimeoutMs * 1'000'000;
};

// The ONE place a policy tag is resolved to the runtime value TcpTransport's constructor takes, e.g.
// `net::TcpTransport t{self, addr, port, fallback_timeout_ns_of<DirectThenRelay<5000>>()};`.
template <class Policy>
[[nodiscard]] constexpr std::int64_t fallback_timeout_ns_of() noexcept {
    return Policy::timeout_ns;
}

}  // namespace quark::net
