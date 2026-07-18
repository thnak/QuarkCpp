// Tests 011-Timers-and-Scheduled-Work §API (delayed send) + §Data structure (ordering, no timer lost
// across wheel rollover / cascade). Uses the externally-driven `tick()` for full determinism — no
// wall-clock sleeps. A 1 ns tick makes "ticks" == the delay counts.
//
// Invariants:
//   1. a delayed `tell` does NOT arrive before its delay, and DOES arrive shortly after it;
//   2. timers due earlier fire before timers due later (arrival order == delay order);
//   3. O(1) insert loses no timer across many level-0 rollovers AND a level-1 cascade.
#include <chrono>
#include <cstdio>

#include "quark/core/timer_service.hpp"
#include "timer_test_util.hpp"

using namespace quark;
using namespace std::chrono_literals;
using timertest::check;

int main() {
    bool ok = true;

    // --- Invariant 1: delayed send fires after ~the delay, not before ---------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 256});  // 1 ns tick, deterministic tick() driving

        auto handle = svc.schedule_after(h.ref(), 50ns, timertest::Fire{7});
        (void)handle;

        svc.advance_ticks(40);                 // well short of the 50-tick delay
        h.flush();                             // settle the mailbox: nothing should have been told
        check(h.delivered() == 0, "delayed timer has not fired before its delay", ok);

        svc.advance_ticks(15);                 // past the delay (+ slack for the wheel fencepost)
        check(h.drain_until(1), "delayed timer fires after ~the delay", ok);
        check(h.delivered() == 1, "delayed timer fires exactly once", ok);
        check(!h.actor().order.empty() && h.actor().order[0] == 7, "correct message delivered", ok);
    }

    // --- Invariants 2 & 3: ordering + no loss across rollover/cascade ---------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 4096});

        // Delays 1..N (N crosses the 64-bucket level-0 rollover repeatedly and forces level-1
        // cascades). Distinct delays ⇒ distinct expiry ticks ⇒ each fires in its own tick, so
        // arrival order must equal delay order.
        constexpr int N = 500;
        for (int d = 1; d <= N; ++d) {
            auto hh = svc.schedule_after(h.ref(), std::chrono::nanoseconds(d), timertest::Fire{d});
            (void)hh;
        }

        svc.advance_ticks(N + 20);             // drive past the largest delay (+ slack)
        check(h.drain_until(N), "every scheduled timer fired (none lost across rollover/cascade)", ok);
        check(h.delivered() == N, "exactly N timers fired", ok);

        bool ordered = (static_cast<int>(h.actor().order.size()) == N);
        for (std::size_t i = 0; ordered && i < h.actor().order.size(); ++i)
            if (h.actor().order[i] != static_cast<int>(i + 1)) ordered = false;
        check(ordered, "earlier-due timers fired before later-due ones (arrival order == delay order)",
              ok);
    }

    std::printf("timer_delayed_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
