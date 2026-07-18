// Implements the shared integer mixer used for placement (026 VirtualBins),
// identity hashing (006/008), and config dispatch (008). Not a cryptographic hash.
#pragma once

#include <cstdint>

namespace quark::detail {

// splitmix64 — the mixer 026 names for `bin = splitmix64(ActorId.hash()) & (B-1)`.
// Deterministic and content-addressed: the same input yields the same output on every node,
// which is exactly what coordinator-free placement requires.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Order-independent 64-bit combine for folding two keys into one (identity, fingerprints).
[[nodiscard]] constexpr std::uint64_t hash_combine(std::uint64_t a, std::uint64_t b) noexcept {
    return splitmix64(a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2)));
}

}  // namespace quark::detail
