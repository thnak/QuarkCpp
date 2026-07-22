// 019-Platform-Abstraction-Layer — Windows/x86-64 backend: the canonical monotonic clock. Mirrors
// pal/linux_x86_64/clock.hpp exactly in shape (same BootClock/WallClock split, same 018/027 reasoning);
// only the OS primitive differs. See that file for the full suspend/wall-clock design rationale.
//
// WHY "UNBIASED INTERRUPT TIME" (018 §Suspend) --------------------------------------------------
// 019 §1 names this the Windows analogue of CLOCK_BOOTTIME: `QueryUnbiasedInterruptTimePrecise`
// reports 100ns ticks since boot INCLUDING time spent in sleep (S1-S3) — "unbiased" means it is not
// adjusted for the RTC/timezone corrections `QueryInterruptTime` can receive across hibernate, not that
// it excludes suspend. That is exactly the CLOCK_BOOTTIME contract 018 needs: a node resuming from
// sleep must see in-flight deadlines as already expired. `QueryPerformanceCounter`, by contrast, is
// NOT guaranteed to keep counting across S3 sleep on all hardware — wrong primitive for this seam.
#pragma once

#include <chrono>
#include <cstdint>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace quark::pal {

// A steady (monotonic, non-decreasing) clock that counts suspend time — unbiased interrupt time on
// Windows. Same named-requirement shape as the Linux BootClock: drop-in for std::chrono::steady_clock
// at the pal::clock seam, duration in nanoseconds.
struct BootClock {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<BootClock, duration>;
    static constexpr bool is_steady = true;

    [[nodiscard]] static time_point now() noexcept {
#if defined(_WIN32)
        ULONGLONG ticks_100ns = 0;  // 100ns units since boot, counts suspend (see banner)
        ::QueryUnbiasedInterruptTimePrecise(&ticks_100ns);
        const auto ns = static_cast<std::int64_t>(ticks_100ns) * 100;
        return time_point(duration(ns));
#else
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::steady_clock::now().time_since_epoch()));
#endif
    }
};

// A WALL-CLOCK (civil/absolute time) source — GetSystemTimePreciseAsFileTime on Windows, converted
// from its 1601 epoch (100ns units) to the Unix (1970) epoch in nanoseconds. Same distinct-domain
// reasoning as pal/linux_x86_64/clock.hpp's WallClock: 027 reminders read this, deadlines never do.
struct WallClock {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<WallClock, duration>;
    static constexpr bool is_steady = false;

    [[nodiscard]] static time_point now() noexcept {
#if defined(_WIN32)
        // Windows FILETIME epoch (1601-01-01) to Unix epoch (1970-01-01), in 100ns units.
        constexpr std::int64_t kFiletimeToUnixEpoch100ns = 116444736000000000LL;
        FILETIME ft{};
        ::GetSystemTimePreciseAsFileTime(&ft);
        const auto ticks_100ns = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) |
                                  static_cast<std::int64_t>(ft.dwLowDateTime);
        const auto ns = (ticks_100ns - kFiletimeToUnixEpoch100ns) * 100;
        return time_point(duration(ns));
#else
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::system_clock::now().time_since_epoch()));
#endif
    }
};

static_assert(!std::is_same_v<BootClock, WallClock>,
              "reminder wall-clock and deadline monotonic clock must be distinct domains (ADR-017)");
static_assert(!WallClock::is_steady && BootClock::is_steady,
              "WallClock follows civil steps (not steady); BootClock is monotonic (steady)");

}  // namespace quark::pal
