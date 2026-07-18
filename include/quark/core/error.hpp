// Implements the engine error model (README language row; 007 failure, 009 observability,
// 008 validation). `result<T>` is the std::expected alias used everywhere off the hot path
// for fallible operations; the hot path stays noexcept and does not allocate to report errors.
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace quark {

// Coarse error categories. The full taxonomy is owned by the specs that raise them
// (007 supervision, 008 validation, 009 dead-letters, 010 distribution); this enum is the
// stable spine they map onto. Extend here as those subsystems land — do not invent ad-hoc
// error enums per module.
enum class errc : std::uint16_t {
    ok = 0,
    cancelled,          // std::stop_token fired (001 cancellation, 015)
    validation,         // metadata/config validation failed (008/013)
    not_found,          // actor/resource/type not resolvable (004/006)
    unavailable,        // node/transport unavailable (010/021)
    overloaded,         // admission/shedding refused the work (022)
    timeout,            // deadline exceeded (011)
    serialization,      // encode/decode/schema mismatch (016)
    supervised_stop,    // handler failed and supervision stopped the actor (007)
    internal,           // invariant violation — a bug, not an expected condition
    circuit_open,       // circuit breaker open (022) — fail fast, no deadline wait; appended to
                        // preserve the existing enum values (never renumber a raised code)
};

// A lightweight, trivially-copyable error value. `detail` is a borrowed static string (a
// literal or interned message) — errors never own heap on the failure path.
struct error {
    errc code = errc::internal;
    std::string_view detail{};

    constexpr error() = default;
    constexpr explicit error(errc c, std::string_view d = {}) noexcept : code(c), detail(d) {}

    constexpr explicit operator bool() const noexcept { return code != errc::ok; }
    friend constexpr bool operator==(const error&, const error&) = default;
};

// The canonical fallible-result type. Handlers, resolution, config, and codecs return this.
template <class T>
using result = std::expected<T, error>;

// Convenience: build an unexpected error in one call — `return fail(errc::not_found, "actor");`
[[nodiscard]] constexpr std::unexpected<error> fail(errc c, std::string_view d = {}) noexcept {
    return std::unexpected<error>(error{c, d});
}

}  // namespace quark
