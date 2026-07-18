// Tests 022 §1 — token-bucket rate limiting: bursts up to the bucket size are admitted, excess is
// shed (Admit::Shed), and the bucket refills over (monotonic) time. O(1), no allocation. Also
// exercises the RateLimiter function-pointer seam (the std-only default binding a TokenBucket).
#include <cassert>
#include <cstdio>

#include "quark/core/governance.hpp"

using namespace quark;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}
constexpr std::int64_t kSec = 1'000'000'000;  // 1 s in ns
}  // namespace

int main() {
    // ---- Burst then shed; refill over time -------------------------------------------------
    {
        TokenBucket b(5.0, 10.0);  // burst 5, 10 tokens/sec
        std::int64_t t = 0;

        // The full bucket admits a burst of 5, then sheds.
        int accepted = 0;
        for (int i = 0; i < 8; ++i)
            if (b.check(t) == Admit::Accept) ++accepted;
        check(accepted == 5, "burst of exactly the bucket size (5) admitted, rest shed");
        check(b.check(t) == Admit::Shed, "empty bucket ⇒ shed");

        // Advance 0.5 s ⇒ +5 tokens (10/s × 0.5 s), capped at burst 5.
        t += kSec / 2;
        accepted = 0;
        for (int i = 0; i < 8; ++i)
            if (b.check(t) == Admit::Accept) ++accepted;
        check(accepted == 5, "half a second refills 5 tokens (capped at burst)");

        // Refill never exceeds the burst cap even after a long idle.
        t += 100 * kSec;
        accepted = 0;
        for (int i = 0; i < 20; ++i)
            if (b.check(t) == Admit::Accept) ++accepted;
        check(accepted == 5, "long idle does not exceed the burst cap");
    }

    // ---- Cost > 1: a heavy message consumes multiple tokens --------------------------------
    {
        TokenBucket b(10.0, 1.0);
        std::int64_t t = 0;
        check(b.check(t, Cost{4}) == Admit::Accept, "cost 4 affordable (10 available)");
        check(b.check(t, Cost{4}) == Admit::Accept, "cost 4 affordable (6 available)");
        check(b.check(t, Cost{4}) == Admit::Shed, "cost 4 unaffordable (2 available) ⇒ shed");
        check(b.check(t, Cost{2}) == Admit::Accept, "cost 2 affordable (2 available)");
    }

    // ---- The RateLimiter seam (function-pointer courier; null ⇒ Accept) --------------------
    {
        TokenBucket b(2.0, 0.0);  // no refill: exactly 2 admissions ever
        RateLimiter rl = as_rate_limiter(b);
        const GovernanceKey key{42};
        check(rl.check(key, Cost{1}, 0) == Admit::Accept, "seam: 1st admitted");
        check(rl.check(key, Cost{1}, 0) == Admit::Accept, "seam: 2nd admitted");
        check(rl.check(key, Cost{1}, 0) == Admit::Shed, "seam: 3rd shed");

        RateLimiter none;  // unwired seam is a no-op Accept (governance is opt-in)
        check(none.check(key, Cost{9}, 0) == Admit::Accept, "null seam ⇒ Accept (opt-in)");
    }

    std::printf("governance_rate_limit_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
