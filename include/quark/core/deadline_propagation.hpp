// Implements 018-Clocks-and-Deadlines §Rule 2 (ship remaining duration, not the instant),
// §Rule 2a (charge SWIM-derived transit to the budget) and §Rule 4 (bias lenient, never falsely
// strict) — the CROSS-NODE half of the deadline model. A monotonic instant from node A (epoch =
// A's boot) is meaningless on node B, so the wire carries a *remaining duration*; B rebases it onto
// its OWN monotonic clock. This header is the pure arithmetic + the seams; it introduces no
// transport, no serializer, no membership dependency.
//
// SEAMS:
//  - 010 transport: at send it calls `encode_deadline_for_wire`; at receive it supplies `now_B`
//    (the receiver's pal::now()) and the transit estimate, then calls `rebase_deadline_from_wire`.
//    The remaining-duration is a SPECIAL-CASED wire field (018 §Rule 2) computed at send /
//    reconstructed at receive — NOT a `describe`d field (016), by design.
//  - 010 SWIM membership: already tracks a smoothed per-peer RTT; `TransitEstimate::from_rtt`
//    turns it into `rtt/2`. No PTP/NTP handshake is introduced (018 self-debate).
//  - 011 wheel: the rebased `Deadline` is just a local instant — an ordinary wheel entry whose
//    expiry behaves identically to a locally-originated deadline.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "pal/pal.hpp"
#include "quark/core/deadline.hpp"

namespace quark {

// --- Transit estimate (018 §Rule 2a / §Rule 4) -----------------------------------------------
// The one-way network delay charged against the budget on arrival. Modelled as a value so the 010
// SWIM RTT feeds in without this header depending on membership. `one_way == 0` means "no estimate
// yet" (first message to a fresh peer): §Rule 4 says subtract NOTHING then — lenient, never
// stricter than the sender intended.
struct TransitEstimate {
    std::chrono::nanoseconds one_way{0};

    // Conservative half-RTT (018 §Rule 2a). Not inflated: a false miss (killing healthy in-flight
    // work) is worse than a few extra ms, so we never round transit UP. A non-positive/absent rtt
    // yields the lenient zero estimate.
    [[nodiscard]] static TransitEstimate from_rtt(std::chrono::nanoseconds rtt) noexcept {
        if (rtt.count() <= 0) return TransitEstimate{};
        return TransitEstimate{std::chrono::nanoseconds(rtt.count() / 2)};
    }
    [[nodiscard]] static constexpr TransitEstimate unknown() noexcept { return TransitEstimate{}; }
};

// --- Send side, node A (018 §Rule 2) ---------------------------------------------------------
// Translate a local absolute deadline into the remaining duration to put on the wire. Returns
// nullopt when there is no deadline (nothing to ship — the peer runs unbounded). The absolute
// instant NEVER crosses the wire; only this duration does. A caller that finds the deadline already
// expired here should dead-letter locally (007/017) rather than send — but we still return the
// (possibly negative) remaining so the decision stays with the transport, not this arithmetic.
[[nodiscard]] inline std::optional<std::chrono::nanoseconds> encode_deadline_for_wire(
    Deadline dl, pal::clock::time_point now_a = pal::now()) noexcept {
    if (!dl.has_value()) return std::nullopt;
    return dl.remaining(now_a);  // signed: <=0 means already-missed in flight
}

// --- Receive side, node B (018 §Rule 2 + §Rule 2a) -------------------------------------------
// Reconstruct a LOCAL absolute deadline on B's monotonic clock:
//     deadline_B = now_B + remaining - transit
// The subtraction charges network flight time to the budget so a multi-hop chain is monotonically
// non-increasing (§Rule 3). With `transit == 0` (unknown peer) the budget is preserved exactly —
// lenient. The result is an ordinary local instant for the 011 wheel; if it lands in the past the
// deadline is already expired (safe: the message spent its whole budget in flight).
[[nodiscard]] inline Deadline rebase_deadline_from_wire(
    std::chrono::nanoseconds remaining, TransitEstimate transit = TransitEstimate::unknown(),
    pal::clock::time_point now_b = pal::now()) noexcept {
    const std::int64_t base = monotonic_ns(now_b);
    const std::int64_t budget = remaining.count() - transit.one_way.count();
    // saturating add so a "never" budget can't wrap negative into a bogus already-expired instant.
    if (budget > 0 && budget > INT64_MAX - base) return Deadline::at_ns(INT64_MAX);
    return Deadline::at_ns(base + budget);
}

}  // namespace quark
