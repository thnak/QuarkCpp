// Tests 011-Timers-and-Scheduled-Work §API (periodic) + §Cancellation. Deterministic tick() driving.
//
// Invariants:
//   1. a periodic timer re-arms and fires N times over N periods;
//   2. cancel() stops all FUTURE fires (a periodic timer stops re-scheduling once cancelled);
//   3. cancel() before a one-shot's due tick prevents the fire entirely (lazy tombstone honored);
//   4. cancel() after a one-shot already fired is a safe no-op (gen-gate — no double effect, no UAF).
#include <chrono>
#include <cstdio>

#include "quark/core/timer_service.hpp"
#include "timer_test_util.hpp"

using namespace quark;
using namespace std::chrono_literals;
using timertest::check;

int main() {
    bool ok = true;

    // --- Invariant 1: periodic re-arm fires N times --------------------------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 256});

        auto p = svc.schedule_every(h.ref(), 10ns, timertest::Fire{1});
        (void)p;

        constexpr int kPeriods = 8;
        svc.advance_ticks(10 * kPeriods + 10);  // 8 periods + slack
        check(h.drain_until(kPeriods), "periodic timer fired at least N times", ok);
        check(h.delivered() >= kPeriods, "periodic re-arm produced >= N fires", ok);
    }

    // --- Invariant 2: cancel stops future periodic fires ---------------------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 256});

        auto p = svc.schedule_every(h.ref(), 10ns, timertest::Fire{2});

        svc.advance_ticks(35);                  // ~3 fires
        h.flush();                              // settle ALL fires before snapshotting the count
        const int before = h.delivered();
        check(before >= 1, "periodic fired before cancel", ok);

        p.cancel();                             // lazy cancel
        svc.advance_ticks(200);                 // many more periods would have elapsed
        h.flush();                              // settle: no new fire must appear
        check(h.delivered() == before, "cancel stopped all future periodic fires", ok);
    }

    // --- Invariant 3: cancel before the due tick prevents the fire -----------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 256});

        auto one = svc.schedule_after(h.ref(), 50ns, timertest::Fire{3});
        svc.advance_ticks(20);                  // still before the 50-tick due point
        one.cancel();
        svc.advance_ticks(60);                  // past the original due point
        h.flush();                              // settle: the fire must not appear
        check(h.delivered() == 0, "cancel before due tick prevented the fire entirely", ok);
    }

    // --- Invariant 4: cancel after fire is a safe no-op (gen-gate) -----------------------------
    {
        timertest::Harness h;
        TimerService svc(TimerService::Config{1ns, 256});

        auto one = svc.schedule_after(h.ref(), 10ns, timertest::Fire{4});
        svc.advance_ticks(30);                  // fires
        h.flush();                              // settle
        check(h.drain_until(1), "one-shot fired", ok);
        const int after = h.delivered();
        one.cancel();                           // stale handle (entry recycled + gen bumped): no-op
        svc.advance_ticks(200);
        h.flush();                              // settle: no further fire
        check(h.delivered() == after, "cancel after fire had no effect (gen-gated no-op)", ok);
        check(after == 1, "one-shot fired exactly once (no double fire)", ok);
    }

    std::printf("timer_periodic_cancel_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
