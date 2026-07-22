// Implements 019-Platform-Abstraction-Layer §Randomness + 020-Security §"Randomness — a PAL concern".
// The ONE canonical cryptographically-secure RNG entry point, so no subsystem (nonces, session keys,
// tokens) reaches for a non-cryptographic `std::mt19937` by accident (020 §Randomness). It is an OS
// service, hence a PAL primitive: `getrandom` on Linux, `BCryptGenRandom` on Windows. macOS
// `getentropy` is a DEFERRED PAL backend, not implemented here.
//
// The SIMULATION backend (014) needs REPRODUCIBILITY, so it must NOT read real OS entropy: install a
// DETERMINISTIC stub via `set_csprng_override` (a seeded splitmix64 stream). This is the ONLY place
// determinism is injected — the stub is emphatically NOT cryptographically secure and is for tests only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "quark/detail/hash.hpp"

#if defined(__linux__)
#include <sys/random.h>  // getrandom(2)
#include <cerrno>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>  // BCryptGenRandom — link bcrypt.lib
#endif

namespace quark::pal {

// A test/simulation override: when non-null, `fill_random` routes through it instead of the OS. The
// sim backend (014) installs a deterministic stream here so a run is reproducible. `ctx` is opaque.
struct CsprngOverride {
    void (*fn)(std::span<std::byte> out, void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
};

// A process-global override slot. Single-writer at setup (before threads start); read on the cold
// key/nonce path only, never on the hot drain path. Not atomic by design — set it once at boot.
inline CsprngOverride& csprng_override() noexcept {
    static CsprngOverride g_override{};
    return g_override;
}

// Install (or clear, with a default-constructed value) the deterministic CSPRNG stub. Cold path.
inline void set_csprng_override(CsprngOverride ov) noexcept { csprng_override() = ov; }

// Fill `out` with cryptographically-secure random bytes. On Linux this is `getrandom(2)` (blocking
// until the pool is initialized, then never failing for a bounded request). If a sim override is
// installed, it is used instead (deterministic, NOT secure). Returns false only on an OS failure with
// no override — callers on the key/nonce path treat that as fatal (cannot proceed without entropy).
[[nodiscard]] inline bool fill_random(std::span<std::byte> out) noexcept {
    const CsprngOverride ov = csprng_override();
    if (ov.fn != nullptr) {
        ov.fn(out, ov.ctx);
        return true;
    }
#if defined(__linux__)
    std::size_t off = 0;
    while (off < out.size()) {
        const ssize_t n = ::getrandom(out.data() + off, out.size() - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted before any bytes — retry
            return false;                  // no entropy source and no override — fatal to the caller
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
#elif defined(_WIN32)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG: let CNG pick the system RNG rather than opening/managing an
    // algorithm provider handle for a call this infrequent (cold key/nonce path only). Returns an
    // NTSTATUS; success is a non-negative value (the usual NT_SUCCESS test).
    const NTSTATUS st = ::BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st >= 0;
#else
    // No verified OS entropy backend on this platform (only linux_x86_64/windows_x86_64 are live). A
    // build for any other OS MUST install a CSPRNG override or it cannot obtain secure randomness.
    // TODO(macos): add a getentropy PAL backend behind this same entry point.
    (void)out;
    return false;
#endif
}

// A deterministic splitmix64 stream stub for the simulation backend (014). Seeded, reproducible, and
// explicitly NOT cryptographically secure — install via `set_csprng_override`. The seed lives in the
// caller-owned `SimCsprngState` so multiple independent streams don't interfere.
struct SimCsprngState {
    std::uint64_t state = 0;
};

inline void sim_csprng_fill(std::span<std::byte> out, void* ctx) noexcept {
    auto* st = static_cast<SimCsprngState*>(ctx);
    std::size_t i = 0;
    while (i < out.size()) {
        st->state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t word = detail::splitmix64(st->state);
        for (int b = 0; b < 8 && i < out.size(); ++b, ++i) {
            out[i] = static_cast<std::byte>(word & 0xFF);
            word >>= 8;
        }
    }
}

// Convenience: build an override bound to a deterministic sim state (test/sim use only).
[[nodiscard]] inline CsprngOverride sim_csprng(SimCsprngState& st) noexcept {
    return CsprngOverride{&sim_csprng_fill, &st};
}

}  // namespace quark::pal
