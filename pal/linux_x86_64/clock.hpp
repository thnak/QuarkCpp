// 019-Platform-Abstraction-Layer — Linux/x86-64 backend: the canonical monotonic clock.
//
// This is the first concrete backend unit under pal/<os>_<arch>/; the wider 019 surface (event
// loop, sockets, NUMA affinity, durable flush) lands here as it is un-blocked. pal.hpp includes
// this and re-exports `BootClock` as the single `pal::clock` seam.
//
// WHY CLOCK_BOOTTIME AND NOT steady_clock (018 §Suspend, resume) --------------------------------
// A Quark deadline is "N ns of elapsed real time", and 018 requires the canonical monotonic clock
// to COUNT the time the machine spends suspended, so a node that was asleep sees its in-flight
// deadlines as ALREADY EXPIRED on resume (the safe outcome — that work is stale). `steady_clock`
// on libstdc++ is `CLOCK_MONOTONIC`, which FREEZES across suspend: a 5-minute suspend with a
// 500 ms deadline live would, on `CLOCK_MONOTONIC`, resume and let the stale work run as if no
// time passed. `CLOCK_BOOTTIME` == `CLOCK_MONOTONIC` + suspend time, so it gives 018's semantics
// exactly. It is VDSO-accelerated on modern x86-64 Linux (measured ~36 ns/read on the dev box,
// on par with CLOCK_MONOTONIC and a hair cheaper than steady_clock), so routing the whole engine
// through it costs the hot path nothing. Epoch = boot, so now() is strictly positive and never 0
// — the invariant `deadline.hpp` leans on for its `0 == none` sentinel.
#pragma once

#include <chrono>
#include <cstdint>
#include <type_traits>

#if defined(__linux__)
#include <ctime>  // ::clock_gettime, CLOCK_BOOTTIME
#endif

namespace quark::pal {

// A steady (monotonic, non-decreasing) clock that counts suspend time — CLOCK_BOOTTIME on Linux.
// Satisfies the C++ `Clock`/`TrivialClock` named requirements, so it is a drop-in for
// `std::chrono::steady_clock` at the `pal::clock` seam: `duration` is nanoseconds and time points
// subtract to nanosecond durations exactly as the deadline/timer/bench code already assumes.
struct BootClock {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<BootClock, duration>;
    static constexpr bool is_steady = true;  // monotonic, never runs backward

    [[nodiscard]] static time_point now() noexcept {
#if defined(__linux__) && defined(CLOCK_BOOTTIME)
        // VDSO fast read on x86-64 Linux — no syscall on the hot path. CLOCK_BOOTTIME advances
        // across machine suspend; that is the whole point (018 §Suspend).
        ::timespec ts{};
        ::clock_gettime(CLOCK_BOOTTIME, &ts);
        const auto ns = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 +
                        static_cast<std::int64_t>(ts.tv_nsec);
        return time_point(duration(ns));
#else
        // Portable fallback (non-Linux, or a kernel without CLOCK_BOOTTIME). This does NOT count
        // suspend — the 019 PAL is hardware-blocked off x86-64 Linux and the suspend-counting
        // guarantee is re-gated with the rest of the cross-platform surface. Keeping a fallback (a
        // #warning, never a hard #error — matching pal.hpp) lets the header still compile for
        // exploration elsewhere without silently pretending suspend is counted.
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::steady_clock::now().time_since_epoch()));
#endif
    }
};

#if !defined(__linux__) || !defined(CLOCK_BOOTTIME)
#warning "Quark PAL clock: CLOCK_BOOTTIME unavailable — falling back to CLOCK_MONOTONIC-class time \
that does NOT count machine suspend. 018 §Suspend deadline semantics are not guaranteed off Linux."
#endif

// A WALL-CLOCK (civil/absolute time) source — CLOCK_REALTIME on Linux. This is a DELIBERATELY
// SEPARATE clock domain from BootClock (027 §"Wall-clock anchoring", ADR-017 C1): 027 durable
// reminders fire at civil time ("21:00"), so they must FOLLOW NTP/DST/admin steps — a backward
// step re-arms a drained segment (absorbed by at-least-once + idempotency), a forward jump fires
// the skipped buckets as catch-up. Deadlines (018) do the OPPOSITE — they read the monotonic
// BootClock and must be IMMUNE to wall-clock steps. Mixing the two domains is the bug this type
// exists to make impossible: a deadline never reads WallClock, a reminder never reads BootClock.
// NOT steady (`is_steady = false`) precisely because it can jump; callers must never do monotonic
// interval math on it. Epoch = Unix epoch (1970), so a WallInstant is comparable to civil time.
struct WallClock {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<WallClock, duration>;
    static constexpr bool is_steady = false;  // follows NTP/DST/admin steps — can jump either way

    [[nodiscard]] static time_point now() noexcept {
#if defined(__linux__)
        ::timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        const auto ns = static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 +
                        static_cast<std::int64_t>(ts.tv_nsec);
        return time_point(duration(ns));
#else
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::system_clock::now().time_since_epoch()));
#endif
    }
};

// The two domains are DISTINCT TYPES so a WallClock time_point can never be silently used where a
// monotonic BootClock instant is expected (and vice versa) — the compiler enforces ADR-017 C1.
static_assert(!std::is_same_v<BootClock, WallClock>,
              "reminder wall-clock and deadline monotonic clock must be distinct domains (ADR-017)");
static_assert(!WallClock::is_steady && BootClock::is_steady,
              "WallClock follows civil steps (not steady); BootClock is monotonic (steady)");

}  // namespace quark::pal
