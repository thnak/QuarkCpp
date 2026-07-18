// Implements 020-Security §3 (principal + attenuating propagation) + §"013 SecurityMode validation" —
// the ambient identity value and its monotonicity rule, plus the config-level Validation invariant.
// This is the umbrella for the std-only security seams; heavy crypto is a DEFERRED adapter (secure_
// transport.hpp / at_rest.hpp name the seam, never implement real crypto).
//
// PRINCIPAL PROPAGATION MIRRORS THE DEADLINE (018), WITH INVERTED MONOTONICITY.
//   * A deadline inherits TIGHTER down the causal chain (a child call cannot outlive its parent —
//     deadline_propagation.hpp).
//   * A principal inherits WEAKER: a handler may substitute a LESSER principal for a downstream send
//     (least-privilege delegation) but the runtime CANNOT forge a STRONGER one. The outbound principal
//     is stamped from the inbound context by INTERSECTING rights, so authority monotonically shrinks —
//     the security analogue of the deadline's monotonic budget shrink (020 §3 self-debate).
// Both ride in the `MessageContext` (004) as ambient per-message values; principal enters at a BOUNDARY
// (ingress / inbound wire) and is carried across a node↔node hop on the `MessageFrame` (transport.hpp).
// A purely intra-process tell crosses no trust boundary and pays nothing (020 core principle) — the
// local drain path never touches a principal (default = anonymous, trivially constructed).
#pragma once

#include <cstddef>

#include "quark/core/engine_config.hpp"  // SecurityMode (013)
#include "quark/core/error.hpp"
#include "quark/core/principal.hpp"  // Principal + attenuation algebra (020 §3)

namespace quark {

// ============================================================================================
// 020 §"013 SecurityMode validation" (008). SecurityMode lives in EngineConfig (013). The load-bearing
// config invariant: under `Strict`, a MULTI-NODE cluster MUST NOT run the plaintext dev transport —
// enabling it there is a startup Validation failure (errc::validation), consistent with the
// `result<…>` model of validate_engine_config. Under `Off` (the single-host / dev default) plaintext is
// allowed and this check is a no-op. `cluster_size` is the configured peer count (1 = single node).
// ============================================================================================
[[nodiscard]] constexpr result<void> validate_security(SecurityMode mode, std::size_t cluster_size,
                                                       bool transport_is_plaintext) noexcept {
    if (mode == SecurityMode::Strict && cluster_size > 1 && transport_is_plaintext) {
        return fail(errc::validation,
                    "Strict + multi-node cluster forbids the plaintext dev transport (020 §2)");
    }
    return {};
}

}  // namespace quark
