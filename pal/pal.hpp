// Implements 019-Platform-Abstraction-Layer (the OS/arch seam) — the OS-selector entry point that
// picks a concrete clock backend (linux_x86_64 or windows_x86_64) at compile time (019 §"Compile-
// time backend selection, not a vtable"). The Linux directory is named linux_x86_64 for history; its
// contents — clock_gettime — are arch-generic Linux code, not x86-specific, and
// this seam is exercised on real arm64 hardware in CI: .github/workflows/ci.yml runs the full
// correctness matrix on GitHub-hosted arm64 runners. Everything OS- or arch-specific in the engine
// routes through this header (or pal/net.hpp, pal/csprng.hpp) so the core stays portable by
// construction. 019's WIDER surface (NUMA affinity) is still unimplemented everywhere (nothing calls
// it yet); the spin hint, monotonic clock, event loop/sockets, CSPRNG, and durable file
// I/O are live on Linux/x86-64 (CI-verified), Linux/arm64 (CI-verified), and Windows/x86-64 (see
// pal/windows_x86_64/) — see CONVENTIONS.md for what "CI-verified" does and does not cover (a
// tool-checked — herd7/GenMC — weak-memory re-gate of the Dekker rendezvous on arm64 is still open).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>  // _mm_pause (pal::cpu_relax) — MSVC has no __builtin_ia32_pause
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>  // __yield (pal::cpu_relax) — MSVC/ARM64 has no GNU `asm volatile` (ADR-059)
#endif

// BootClock/WallClock — the canonical suspend-counting monotonic clock + civil wall clock, one
// concrete backend per OS (019 §"Per-OS backends"). Compile-time selection, not a vtable (019
// §"Compile-time backend selection, not a vtable").
#if defined(_WIN32) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include "pal/windows_arm64/clock.hpp"  // ADR-059: thin arch-selector over the arch-generic Windows code
#elif defined(_WIN32)
#include "pal/windows_x86_64/clock.hpp"
#else
#include "pal/linux_x86_64/clock.hpp"  // also the POSIX/Linux fallback used elsewhere in this header
#endif
#include "quark/core/config.hpp"

#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#warning "Quark PAL: only the x86-64 and arm64 backends are covered by CI. Other arches need a \
cpu_relax() review and their own weak-memory re-gate (see decisions/ADR-002, ADR-004, ADR-015, ADR-066)."
#endif

namespace quark::pal {

// Platform tag carried in the 016 wire negotiation (tagless fast path is safe only between peers
// whose ABI/endianness tag matches; a mismatch safely falls back to the tagged/TLV path — ADR-016).
// Windows gets its OWN tag rather than reusing Linux's 'L': both are x86-64 little-endian, but this
// tag has not been proven to gate on struct-layout/ABI equivalence across the two toolchains (MSVC
// vs GCC/Clang), so the conservative default is "no tagless fast path between them yet" until that
// is checked and the two are deliberately unified.
#if defined(_WIN32) && (defined(_M_ARM64) || defined(_M_ARM64EC))
// Windows/ARM64: its own tag (ADR-059) — not x86, and never assumed struct/ABI-equivalent to the
// x86-64 tags, so peers fall back to the tagged/TLV path (016) until that is proven.
inline constexpr std::uint32_t platform_abi_tag = 0x41'36'34'57;  // 'A','64','W' (Windows/ARM64)
#elif defined(_WIN32)
inline constexpr std::uint32_t platform_abi_tag = 0x78'86'64'57;  // 'x','86','64','W' (Windows)
#else
inline constexpr std::uint32_t platform_abi_tag = 0x78'86'64'4C;  // 'x','86','64','L' (Linux)
#endif

// --- Memory ordering ----------------------------------------------------------
//
// There is deliberately NO standalone StoreLoad-barrier seam here any more (ADR-066). Every Dekker
// rendezvous in the engine (002 wakeup + drain-owner, 024 streaming arm/un-stall, 015 Parked/resume)
// is spelled as seq_cst atomic OPERATIONS on both halves — a seq_cst store/RMW followed by a seq_cst
// load — which the C++ memory model alone makes correct on every ISA, which the compiler lowers to
// the cheapest per-arch form (x86-64: `xchg`/locked RMW + plain `mov`; AArch64: `stlr`/`swpal` +
// `ldar`, no `dmb`), and which ThreadSanitizer models (it does not model `atomic_thread_fence`).

// A cheap spin-wait hint for bounded busy-poll loops (ADR-035's pre-park spin, ADR-036's
// post-drain linger, the mailbox/run-queue Busy tri-state spins). Not a memory barrier — purely a
// low-power/hyperthread-yield instruction, safe to call any number of times. x86: `PAUSE`. AArch64:
// `YIELD` (a real hardware hint, not a compiler-only fence — ADR-036 found that emitting only a
// compiler barrier here on non-x86 would have widened a documented gap onto a newly HOT call site).
// Any other arch falls back to a compiler barrier only — still correct (the loop's own re-check is
// what matters for progress), just without the low-power hint.
QUARK_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();  // MSVC has no __builtin_ia32_pause; this is its PAUSE-instruction intrinsic
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(_MSC_VER) && !defined(__clang__) && (defined(_M_ARM64) || defined(_M_ARM64EC))
    __yield();  // MSVC ARM64 intrinsic for the AArch64 YIELD hint (no GNU inline asm on MSVC)
#elif defined(__aarch64__) || defined(_M_ARM64)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// --- Clock ------------------------------------------------------------------
//
// The canonical monotonic time source (011 timer wheel, 018 deadlines, 023 benchmarks). It is
// CLOCK_BOOTTIME-class (`BootClock`, pal/linux_x86_64/clock.hpp or pal/windows_x86_64/clock.hpp),
// i.e. monotonic AND counting the time the machine spends suspended, so a node resuming from
// suspend sees in-flight deadlines as already expired — the safe, stale-work outcome 018 §Suspend
// requires. All deadline/timer
// arithmetic is written against `pal::now()`, so it inherits this for free. Deliberately NOT
// `std::chrono::steady_clock`: on libstdc++ that is CLOCK_MONOTONIC, which freezes across suspend.
// The full clock seam (coarse vs precise, 014 sim backend) is 019/014.
using clock = BootClock;

[[nodiscard]] inline clock::time_point now() noexcept { return clock::now(); }

// --- Wall clock (civil/absolute time) ---------------------------------------
//
// A SECOND, distinct clock domain — CLOCK_REALTIME-class (`WallClock`, per-OS clock.hpp), for 027
// durable reminders that fire at civil time ("21:00") and must FOLLOW NTP/DST/admin steps.
// This is the opposite requirement from `now()`/deadlines (which must be immune to such steps), so
// the two are separate types the compiler keeps apart (ADR-017 C1). Reminders are the ONLY core
// subsystem that reads this; nothing on a deadline/timer path may. See 027 §"Wall-clock anchoring".
using wall_clock = WallClock;

[[nodiscard]] inline wall_clock::time_point wall_now() noexcept { return wall_clock::now(); }

}  // namespace quark::pal
