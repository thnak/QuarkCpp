// Tests 022 §3 — the circuit breaker bounding a downstream `ask` pileup. Closed → Open (fail fast
// after a consecutive-failure threshold) → Half-Open (one probe after a cooldown) → Closed on a
// successful probe / re-Open on a failing one. While Open, sends fail immediately (Admit::Shed ⇒
// errc::circuit_open) instead of waiting out a deadline. Deterministic under a fake monotonic clock.
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
using State = CircuitBreaker::State;
}  // namespace

int main() {
    constexpr std::uint32_t kThreshold = 3;
    constexpr std::int64_t kOpenNs = 1'000;  // cooldown in fake-clock ns

    // ---- Trip after the threshold, then fail fast while Open --------------------------------
    {
        CircuitBreaker cb(kThreshold, kOpenNs);
        std::int64_t t = 0;

        // Closed: sends are accepted.
        check(cb.on_send(t) == Admit::Accept, "closed: accept");
        check(cb.state() == State::Closed, "starts Closed");

        // Feed consecutive failures up to the threshold.
        cb.on_result(false, t);
        cb.on_result(false, t);
        check(cb.state() == State::Closed, "below threshold: still Closed");
        cb.on_result(false, t);  // 3rd consecutive failure ⇒ trip
        check(cb.state() == State::Open, "threshold reached: Open");

        // While Open (before cooldown), every send fails fast.
        check(cb.on_send(t + 1) == Admit::Shed, "open: shed (fail fast)");
        check(cb.on_send(t + 500) == Admit::Shed, "open: still shed within cooldown");
    }

    // ---- Half-open probe recovers the circuit ----------------------------------------------
    {
        CircuitBreaker cb(kThreshold, kOpenNs);
        for (std::uint32_t i = 0; i < kThreshold; ++i) cb.on_result(false, 0);
        check(cb.is_open(), "tripped Open");

        // Before the cooldown: shed.
        check(cb.on_send(kOpenNs - 1) == Admit::Shed, "pre-cooldown: shed");
        // After the cooldown: exactly one half-open probe is admitted.
        check(cb.on_send(kOpenNs) == Admit::Accept, "cooldown elapsed: one probe admitted");
        check(cb.state() == State::HalfOpen, "probe ⇒ Half-Open");
        check(cb.on_send(kOpenNs) == Admit::Shed, "half-open: only one probe in flight");

        // The probe succeeds ⇒ circuit closes and normal traffic resumes.
        cb.on_result(true, kOpenNs);
        check(cb.state() == State::Closed, "successful probe ⇒ Closed");
        check(cb.on_send(kOpenNs + 1) == Admit::Accept, "recovered: accept");
    }

    // ---- A failing half-open probe re-opens the circuit ------------------------------------
    {
        CircuitBreaker cb(kThreshold, kOpenNs);
        for (std::uint32_t i = 0; i < kThreshold; ++i) cb.on_result(false, 0);
        check(cb.on_send(kOpenNs) == Admit::Accept, "half-open probe admitted");
        cb.on_result(false, kOpenNs);  // probe still fails
        check(cb.state() == State::Open, "failed probe ⇒ re-Open");
        check(cb.on_send(kOpenNs + 1) == Admit::Shed, "re-open: shed again");
        // A second cooldown allows another probe.
        check(cb.on_send(2 * kOpenNs) == Admit::Accept, "second cooldown ⇒ new probe");
    }

    // ---- A success resets the consecutive-failure count (no premature trip) ----------------
    {
        CircuitBreaker cb(kThreshold, kOpenNs);
        cb.on_result(false, 0);
        cb.on_result(false, 0);
        cb.on_result(true, 0);   // success resets the streak
        cb.on_result(false, 0);
        cb.on_result(false, 0);
        check(cb.state() == State::Closed, "intermittent failures below threshold stay Closed");
    }

    std::printf("governance_circuit_breaker_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
