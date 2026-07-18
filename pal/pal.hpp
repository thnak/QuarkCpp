// Implements 019-Platform-Abstraction-Layer (the OS/arch seam) — Linux/x86-64 backend only.
// Everything OS- or arch-specific in the engine routes through this header so the core stays
// portable by construction. 019 is Draft/hardware-blocked for non-x86-64; the wider surface
// (event loop, sockets, NUMA affinity, durable flush) lands with that spec. What the *core*
// hot path needs today is the memory-ordering seam and a monotonic clock, provided here.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "pal/linux_x86_64/clock.hpp"  // BootClock — the canonical suspend-counting monotonic clock
#include "quark/core/config.hpp"

#if !defined(__x86_64__) && !defined(_M_X64)
#warning "Quark PAL: only the x86-64 backend exists. Other arches need a store_load_barrier() \
review and an ARM64 weak-memory re-gate (see decisions/ADR-002, ADR-004, ADR-015)."
#endif

namespace quark::pal {

// Platform tag carried in the 016 wire negotiation (tagless fast path is safe only between
// peers whose ABI/endianness tag matches). x86-64 Linux little-endian is the one live value.
inline constexpr std::uint32_t platform_abi_tag = 0x78'86'64'4C;  // 'x','86','64','L'

// --- Memory ordering seam ---------------------------------------------------
//
// A full StoreLoad barrier — the ISA-independent primitive the Dekker close-out rendezvous
// (002 wakeup, 024 streaming, 015 Parked/resume) is spelled against. `atomic_thread_fence`
// lowers to the correct instruction per arch: `mfence` on x86-64, `dmb ish` on AArch64.
//
// PRODUCER-SIDE ELISION (ADR-004): on x86-TSO a producer's `tail_.exchange(acq_rel)` is
// itself a full StoreLoad barrier, so the producer half of the rendezvous ELIDES this call to
// zero instructions. That elision is x86-only and is a decision made *at the mailbox*, not
// here — this seam always emits a real barrier. // TODO(arm64): a real `dmb ish` is retained
// on AArch64; re-gate the whole close-out under a weak-memory model before promoting.
QUARK_ALWAYS_INLINE void store_load_barrier() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

// --- Clock ------------------------------------------------------------------
//
// The canonical monotonic time source (011 timer wheel, 018 deadlines, 023 benchmarks). It is
// CLOCK_BOOTTIME-class (`BootClock`, pal/linux_x86_64/clock.hpp), i.e. monotonic AND counting the
// time the machine spends suspended, so a node resuming from suspend sees in-flight deadlines as
// already expired — the safe, stale-work outcome 018 §Suspend requires. All deadline/timer
// arithmetic is written against `pal::now()`, so it inherits this for free. Deliberately NOT
// `std::chrono::steady_clock`: on libstdc++ that is CLOCK_MONOTONIC, which freezes across suspend.
// The full clock seam (coarse vs precise, 014 sim backend) is 019/014.
using clock = BootClock;

[[nodiscard]] inline clock::time_point now() noexcept { return clock::now(); }

// --- Wall clock (civil/absolute time) ---------------------------------------
//
// A SECOND, distinct clock domain — CLOCK_REALTIME-class (`WallClock`, pal/linux_x86_64/clock.hpp),
// for 027 durable reminders that fire at civil time ("21:00") and must FOLLOW NTP/DST/admin steps.
// This is the opposite requirement from `now()`/deadlines (which must be immune to such steps), so
// the two are separate types the compiler keeps apart (ADR-017 C1). Reminders are the ONLY core
// subsystem that reads this; nothing on a deadline/timer path may. See 027 §"Wall-clock anchoring".
using wall_clock = WallClock;

[[nodiscard]] inline wall_clock::time_point wall_now() noexcept { return wall_clock::now(); }

}  // namespace quark::pal
