// Implements 020-Security §3 + ADR-044 — the out-of-line tail struct a wire-arrived (or ambiently
// non-anonymous-principal) Descriptor carries. Reached ONLY when Descriptor::gen_state's flags word
// has kControlFlagHasEnvelope set (descriptor.hpp). Never a Descriptor member — Descriptor stays
// byte-for-byte unchanged, 56 B (ADR-044 C5).
#pragma once

#include <type_traits>

#include "quark/core/principal.hpp"

namespace quark::detail {

struct DescriptorEnvelope {
    Principal principal;
};
static_assert(std::is_trivially_copyable_v<DescriptorEnvelope>);

}  // namespace quark::detail
