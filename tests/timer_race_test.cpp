// Tests 011-Timers-and-Scheduled-Work §Cancellation — the fired-vs-cancel RACE. A canceller thread
// races the tick/fire thread over the same one-shot timers, across many rounds. The gen-gated,
// mutex-serialized discipline (timer_service.hpp) must make every timer resolve to EXACTLY ONE
// outcome: no double-fire, no lost cancel corruption, no use-after-free. Built to be run under
// ASan/UBSan and TSan — the cross-thread edge (cancel ↔ fire ↔ recycle) is the whole point.
//
// Assertion: no message id is ever delivered twice (no double fire), delivered == order size, and —
// under the sanitizers — no data race / UAF is reported. (Whether a given timer fired or was
// cancelled is inherently nondeterministic; "at most once, cleanly" is the invariant.)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_set>
#include <vector>

#include "quark/core/timer_service.hpp"
#include "timer_test_util.hpp"

using namespace quark;
using namespace std::chrono_literals;
using timertest::check;

int main() {
    bool ok = true;

    timertest::Harness h;
    TimerService svc(TimerService::Config{1ns, 8192});

    constexpr int kRounds = 200;
    constexpr int kPerRound = 32;
    int next_id = 1;

    for (int r = 0; r < kRounds; ++r) {
        std::vector<TimerHandle> handles;
        handles.reserve(kPerRound);
        for (int i = 0; i < kPerRound; ++i)
            handles.push_back(svc.schedule_after(h.ref(), 1ns, timertest::Fire{next_id++}));

        // Race: one thread cancels every handle while the main thread advances the wheel past the
        // timers' due tick (firing whatever survives). Both funnel through the service mutex, so the
        // gen-gate decides each timer's single outcome.
        std::atomic<bool> go{false};
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire)) { /* line up on the start gate */ }
            for (auto& hd : handles) hd.cancel();
        });

        go.store(true, std::memory_order_release);
        svc.advance_ticks(4);  // drive past the 1-tick due point (+ slack) while cancels race
        canceller.join();
    }

    // Every fire was `tell`ed from THIS thread during advance_ticks(); a Sentinel flush settles them.
    h.flush();

    // No id may appear twice — that would be a double fire (a fire-after-recycle or lost gen-gate).
    std::unordered_set<int> seen;
    bool no_dup = true;
    for (int id : h.actor().order)
        if (!seen.insert(id).second) no_dup = false;
    check(no_dup, "no timer fired twice under the cancel/fire race (no double-fire, no UAF)", ok);
    check(static_cast<std::size_t>(h.delivered()) == h.actor().order.size(),
          "delivered count matches recorded fires", ok);
    check(h.delivered() <= kRounds * kPerRound, "fires never exceed timers scheduled", ok);

    std::printf("timer_race_test: %s  (scheduled=%d, fired=%d)\n", ok ? "OK" : "FAIL",
                kRounds * kPerRound, h.delivered());
    return ok ? 0 : 1;
}
