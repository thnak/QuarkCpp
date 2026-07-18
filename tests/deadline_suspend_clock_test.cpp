// Tests the 018 §Suspend / 019 PAL clock hook: pal::clock is CLOCK_BOOTTIME-class (counts machine
// suspend), so a node resuming from suspend sees in-flight deadlines as already expired — the safe,
// stale-work outcome. Four things are proven:
//   1. pal::clock is STEADY and MONOTONIC non-decreasing, epoch positive (the 0==none invariant).
//   2. pal::now() is wired to CLOCK_BOOTTIME specifically (bracketed by two direct BOOTTIME reads).
//      On a machine that was ever suspended, CLOCK_MONOTONIC lags BOOTTIME by the suspend time, so
//      this bracket would FAIL if the seam still read steady_clock — a real discriminator, not a
//      tautology. (Vacuously true, still passing, on a never-suspended machine.)
//   3. BOOTTIME is never behind MONOTONIC (boottime = monotonic + suspended_time, suspended >= 0).
//   4. SEMANTIC suspend outcome: a deadline stamped before a (simulated) suspend reads as expired on
//      resume — modeled deterministically by advancing the injected `now` by the suspend duration,
//      which is exactly what CLOCK_BOOTTIME does across a real sleep.
#include <chrono>
#include <cstdio>
#include <type_traits>

#if defined(__linux__) && defined(CLOCK_BOOTTIME)
#include <ctime>
#endif

#include "pal/pal.hpp"
#include "quark/core/deadline.hpp"
#include "quark/core/error.hpp"

using namespace quark;
using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// --- 1. pal::clock is a steady, monotonic, positive-epoch clock ------------------------------
void test_clock_is_steady_monotonic() {
    static_assert(pal::clock::is_steady, "pal::clock must be steady (monotonic, never backward)");
    static_assert(std::is_same_v<pal::clock::duration, std::chrono::nanoseconds>,
                  "pal::clock::duration must be nanoseconds so deadline/timer ns arithmetic is exact");

    const auto a = pal::now();
    check(a.time_since_epoch().count() > 0, "pal::now() is strictly positive (epoch=boot; 0==none)");
    check(monotonic_now_ns() > 0, "monotonic_now_ns() > 0 (deadline 0==none sentinel is safe)");

    auto prev = pal::now();
    for (int i = 0; i < 100000; ++i) {
        const auto cur = pal::now();
        if (cur < prev) { check(false, "pal::now() ran backward (not monotonic)"); return; }
        prev = cur;
    }
    check(true, "pal::now() is monotonic non-decreasing over 100k reads");
}

// --- 2 & 3. pal::now() reads CLOCK_BOOTTIME, which is never behind CLOCK_MONOTONIC -----------
void test_clock_is_boottime() {
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
    auto read_ns = [](clockid_t id) -> std::int64_t {
        ::timespec ts{};
        ::clock_gettime(id, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    };

    // Bracket a pal::now() read between two direct CLOCK_BOOTTIME reads. Because all three sample the
    // same counter, pal MUST fall inside — UNLESS pal reads a different clock offset from BOOTTIME
    // (e.g. steady_clock==MONOTONIC on a box that was suspended), which would push it out of the
    // bracket. This is the discriminating proof the seam points at BOOTTIME, not MONOTONIC.
    const std::int64_t boot_before = read_ns(CLOCK_BOOTTIME);
    const std::int64_t pal_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(pal::now().time_since_epoch()).count();
    const std::int64_t boot_after = read_ns(CLOCK_BOOTTIME);
    check(boot_before <= pal_ns && pal_ns <= boot_after,
          "pal::now() is bracketed by CLOCK_BOOTTIME reads (the seam is wired to BOOTTIME)");

    // boottime = monotonic + suspended_time, and suspended_time >= 0, so BOOTTIME is never behind
    // MONOTONIC. (Equal on a never-suspended box; strictly ahead once it has slept.)
    const std::int64_t mono = read_ns(CLOCK_MONOTONIC);
    const std::int64_t boot = read_ns(CLOCK_BOOTTIME);
    check(boot + 5'000'000 >= mono,  // 5ms slack for the inter-read gap
          "CLOCK_BOOTTIME is not behind CLOCK_MONOTONIC");
#else
    std::printf("  (skip: no CLOCK_BOOTTIME on this platform — fallback clock in use)\n");
    check(true, "boottime check skipped off Linux");
#endif
}

// --- 4. Semantic suspend outcome: pre-suspend deadline expires on resume ---------------------
void test_deadline_expires_across_suspend() {
    // A live 500ms deadline stamped just before the machine suspends.
    const pal::clock::time_point t_before_suspend = pal::now();
    const Deadline dl = Deadline::after(500ms, t_before_suspend);
    check(!dl.expired(t_before_suspend), "deadline live at stamp time");
    check(!dl.expired(t_before_suspend + 400ms), "deadline still live 400ms in");

    // The machine suspends for 5 minutes. On CLOCK_BOOTTIME, now() on resume has advanced by the
    // full suspend duration — modeled here by advancing the injected instant by 5 minutes. The
    // 500ms deadline is now far in the past => expired, and check_deadline yields errc::timeout.
    const pal::clock::time_point t_on_resume = t_before_suspend + 5min;
    check(dl.expired(t_on_resume), "deadline expired on resume from a 5-minute suspend");

    const result<void> verdict = check_deadline(dl, t_on_resume);
    check(!verdict.has_value() && verdict.error().code == errc::timeout,
          "check_deadline reports errc::timeout on resume (stale work rejected)");

    // Contrast: had pal::clock been CLOCK_MONOTONIC (frozen across suspend), now() on resume would
    // read ~t_before_suspend and the stale work would run as if no time passed. That is the bug the
    // CLOCK_BOOTTIME choice fixes; this test documents the correct post-fix behavior.
}

}  // namespace

int main() {
    test_clock_is_steady_monotonic();
    test_clock_is_boottime();
    test_deadline_expires_across_suspend();
    if (failures == 0) {
        std::printf("deadline_suspend_clock_test: all checks passed\n");
        return 0;
    }
    std::printf("deadline_suspend_clock_test: %d FAILED\n", failures);
    return 1;
}
