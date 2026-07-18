// Implements 020-Security §3 — the ambient `Principal` identity value and its attenuation algebra.
// Split into a MINIMAL header (only <cstdint>) so `message_context.hpp` (004, hot path) can carry a
// principal alongside the deadline/trace WITHOUT pulling the config/validation surface. The higher-
// level security seams (validate_security, the sinks) live in security.hpp on top of this.
//
// MONOTONICITY (mirrors the deadline, inverted): a deadline inherits TIGHTER down the causal chain;
// a principal inherits WEAKER. The outbound principal is stamped by INTERSECTING rights, so authority
// only ever shrinks — a handler can delegate LESS but can never forge MORE (020 §3).
#pragma once

#include <cstdint>

namespace quark {

// A security principal: an identity plus the rights it may exercise (020 §3). Trivially copyable,
// default = ANONYMOUS with no rights. `rights` is a capability bitset whose SUBSET relation is the
// attenuation order. v1 is ACL-shaped; the capability direction (a ref IS the permission) is a DEFERRED
// seam (020 §3 self-debate).
struct Principal {
    std::uint64_t subject = 0;  // identity (0 = anonymous / unauthenticated)
    std::uint64_t rights = 0;   // capability bitset — SUBSET is the attenuation order

    friend constexpr bool operator==(const Principal&, const Principal&) = default;

    [[nodiscard]] constexpr bool anonymous() const noexcept { return subject == 0 && rights == 0; }
};

// `holder` dominates `other`: every right `other` holds, `holder` holds too (the partial order).
[[nodiscard]] constexpr bool dominates(const Principal& holder, const Principal& other) noexcept {
    return (other.rights & ~holder.rights) == 0;
}

// `requested` asks for a right `inbound` lacks — an AMPLIFICATION attempt the runtime must refuse.
[[nodiscard]] constexpr bool is_amplification(const Principal& inbound,
                                              const Principal& requested) noexcept {
    return (requested.rights & ~inbound.rights) != 0;
}

// Stamp the outbound principal for a downstream send from the inbound context (020 §3). Rights are
// INTERSECTED with the inbound principal's, so the result can NEVER carry a right the inbound lacked —
// attenuation is mechanical. Requesting MORE is silently clamped (detectable via `is_amplification`).
[[nodiscard]] constexpr Principal attenuate(const Principal& inbound,
                                            const Principal& requested) noexcept {
    return Principal{requested.subject, inbound.rights & requested.rights};
}

// "Inherit the inbound principal unchanged" — the default downstream send (no relabel, full authority).
[[nodiscard]] constexpr Principal inherit(const Principal& inbound) noexcept { return inbound; }

}  // namespace quark
