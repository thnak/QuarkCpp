// Implements 022 §admission / 020 §3 — the fixed-size per-sender token bucket shared by the SWIM
// control-plane budget (cluster.hpp) and the per-peer data-plane budget (distribution.hpp).
// Review E2: the refill must advance the clock only by the time converted into WHOLE tokens. Moving
// `last_ns` to `now` when integer division yields 0 discards the fraction, so a sender faster than
// 1/rate never refills and is starved to ~zero instead of limited to `rate`.
#pragma once

#include <algorithm>
#include <cstdint>

namespace quark::detail {

struct RateBucket {
    std::int64_t last_ns = 0;  // 0 = never used (first call grants a full burst)
    std::uint64_t tokens = 0;

    // Take one token. `rate` tokens/second (> 0), capacity `burst` (>= 1). Returns false when empty.
    [[nodiscard]] bool admit(std::int64_t now, std::uint64_t rate, std::uint64_t burst) noexcept {
        if (last_ns == 0) {
            last_ns = now;
            tokens = burst;
        } else if (now > last_ns) {
            const std::int64_t elapsed = now - last_ns;
            if (elapsed >= 1'000'000'000LL) {
                tokens = burst;  // a full second idle refills the bucket
                last_ns = now;
            } else {
                const std::uint64_t add = static_cast<std::uint64_t>(elapsed) * rate / 1'000'000'000ULL;
                if (add > 0) {
                    const std::uint64_t want = tokens + add;
                    // Only a refill CLIPPED by the burst cap discards time (the bucket was full);
                    // otherwise keep the fractional remainder for the next call.
                    last_ns = want > burst
                                  ? now
                                  : last_ns + static_cast<std::int64_t>(add * 1'000'000'000ULL / rate);
                    tokens = std::min(burst, want);
                }
            }
        }
        if (tokens == 0) return false;
        --tokens;
        return true;
    }
};

}  // namespace quark::detail
