// Tests 014-Testing-Model §virtual clock — the fix for the audit's proven wall-clock leak: a
// MaxRestarts<N, Within<W>> supervision window must read the SimEngine's VIRTUAL clock, not the host
// steady clock. Before the fix, whether an always-faulting actor escalated depended on real machine
// speed (flaky); after it, the outcome is a deterministic function of the seed + advance() calls.
//
// Policy: Restart, at most 2 restarts per 100 ms window; exceeding ⇒ escalate → Stop. We deliver 5
// always-faulting messages, one per run_until_idle, under three regimes that differ ONLY in time:
//   * no-gap          → all 5 faults land at virtual t=0 ⇒ inside one window ⇒ escalate + stop.
//   * virtual-advance → advance(150 ms) between faults ⇒ each in a fresh window ⇒ NEVER escalates.
//   * real-sleep      → sleep(150 ms) between faults. With the fix this is IGNORED (virtual time did
//                       not move), so it MUST match no-gap — that equality is the anti-leak guard.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

#include "quark/core/actor.hpp"
#include "quark/core/sim_scheduler.hpp"
#include "quark/core/supervision.hpp"

using namespace quark;
using namespace std::chrono_literals;

namespace {

struct Boom {};
struct AlwaysBoom : Actor<AlwaysBoom, Sequential, OnFailure<Restart, MaxRestarts<2, Within<100>>>> {
    using protocol = Protocol<Boom>;
    void handle(const Boom&) { throw std::runtime_error("boom"); }
};

struct Out {
    std::uint32_t restarts;
    std::uint32_t escalations;
    int stopped;
    friend bool operator==(const Out&, const Out&) = default;
};

Out run(bool virtual_advance, bool real_sleep) {
    SimEngine sim{42};  // SAME seed in every arm — any difference is pure time behavior
    auto ref = sim.spawn<AlwaysBoom>(1);
    for (int i = 0; i < 5; ++i) {
        ref.tell(Boom{});
        sim.run_until_idle();
        if (virtual_advance) sim.advance(150ms);
        if (real_sleep) std::this_thread::sleep_for(150ms);
    }
    auto& a = sim.activation<AlwaysBoom>(1);
    return Out{a.restarts_total(), a.escalations(), a.is_stopped() ? 1 : 0};
}

}  // namespace

int main() {
    bool ok = true;
    const Out none = run(/*virtual*/ false, /*real*/ false);
    const Out vadv = run(/*virtual*/ true, /*real*/ false);
    const Out rslp = run(/*virtual*/ false, /*real*/ true);

    // no-gap: all faults in one virtual window ⇒ budget of 2 exceeded ⇒ escalate + stop.
    if (!(none.restarts == 2 && none.escalations == 1 && none.stopped == 1)) {
        std::fprintf(stderr, "  FAIL: no-gap expected restarts=2/esc=1/stopped=1, got %u/%u/%d\n",
                     none.restarts, none.escalations, none.stopped);
        ok = false;
    }
    // virtual advance resets the window each round ⇒ never escalates (the window IS virtual-driven).
    if (!(vadv.restarts == 5 && vadv.escalations == 0 && vadv.stopped == 0)) {
        std::fprintf(stderr, "  FAIL: virtual-advance expected restarts=5/esc=0/stopped=0, got %u/%u/%d\n",
                     vadv.restarts, vadv.escalations, vadv.stopped);
        ok = false;
    }
    // THE ANTI-LEAK GUARD: real wall-clock sleep must NOT change the outcome (virtual time didn't move).
    if (!(rslp == none)) {
        std::fprintf(stderr, "  FAIL: real-sleep leaked into the window (got %u/%u/%d, expected == no-gap)\n",
                     rslp.restarts, rslp.escalations, rslp.stopped);
        ok = false;
    }
    // Determinism: a second no-gap run with the same seed is identical.
    if (!(run(false, false) == none)) {
        std::fprintf(stderr, "  FAIL: same-seed run not reproducible\n");
        ok = false;
    }

    std::printf("sim_virtual_clock_window_test: %s  (no-gap=%u/%u/%d vadv=%u/%u/%d rslp=%u/%u/%d)\n",
                ok ? "OK" : "FAIL", none.restarts, none.escalations, none.stopped, vadv.restarts,
                vadv.escalations, vadv.stopped, rslp.restarts, rslp.escalations, rslp.stopped);
    return ok ? 0 : 1;
}
