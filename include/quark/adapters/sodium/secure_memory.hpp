// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" and AeroCoWorker ADR-047 D3 / issue #64 — locked,
// wiped storage for the pinned path's LONG-LIVED secrets, and a scope guard that scrubs a temporary on every exit.
//
// WHY LOCKED STORAGE IS LIMITED TO THE IDENTITY: `sodium_malloc` rounds every allocation up to a whole page and
// `mlock`s it, so one key copy costs one locked page. Locking each per-handshake buffer (ephemeral secret, chaining
// key, cipher key, and Split's two transport keys) would cost ~7 pages per handshake while up to 256 handshakes are
// parked, and the locked total would exceed a default 64 KiB RLIMIT_MEMLOCK (what a Docker container gives) after a
// handful of concurrent handshakes. Failing closed there would turn a handshake flood into an outage of honest
// handshakes — the exact property tests/adapters/pinned_flood_check.cpp F1 asserts. So:
//   * the identity's Ed25519 seed and its derived X25519 static key live here, are ALWAYS locked, and a lock that
//     fails refuses the identity (fail closed, as the application's custody claim requires);
//   * every per-handshake and per-session key copy is ordinary memory that is WIPED on every exit path
//     (ScopedWipe below, plus the overwrite wipes in noise.hpp), with the swap/dump residual documented at the
//     per-session cipher in sodium_aead.hpp.
#pragma once

#include <sodium.h>

#include <cstddef>
#include <span>

namespace quark::adapters {

// Non-elidable zeroing. sodium_memzero is SecureZeroMemory on Windows, explicit_bzero/memset_s where available, and
// a volatile loop otherwise, so the compiler may not drop it.
inline void secure_wipe(std::span<std::byte> s) noexcept {
    if (!s.empty()) ::sodium_memzero(s.data(), s.size());
}

namespace detail {
// Test seam for the "a failed lock is refused" control (#64): when non-null it replaces the sodium_mlock call, so a
// test can force a lock failure without an unprivileged way to exhaust RLIMIT_MEMLOCK. Never set in production.
inline int (*mlock_probe_for_test)(void*, std::size_t) = nullptr;
}  // namespace detail

// A heap block from sodium_malloc (guard pages + canary) whose mlock result is CHECKED — unlike sodium_malloc itself,
// which ignores a failed mlock (libsodium 1.0.18/1.0.22, src/libsodium/sodium/utils.c: the return of sodium_mlock is
// discarded). If the allocation or the lock fails, the block is released and ok() is false: callers must fail closed.
//
// Move-only. The destructor zeroes the bytes, unlocks and frees; a moved-from object owns nothing.
class LockedBuffer {
public:
    LockedBuffer() noexcept = default;

    explicit LockedBuffer(std::size_t n) noexcept : n_(n) {
        p_ = ::sodium_malloc(n_ == 0 ? 1 : n_);
        if (p_ == nullptr) {
            n_ = 0;
            return;
        }
        secure_wipe(span());
        const int rc = detail::mlock_probe_for_test ? detail::mlock_probe_for_test(p_, n_) : ::sodium_mlock(p_, n_);
        if (rc != 0) {  // sodium_malloc ignores this failure; we do not: refuse the buffer
            ::sodium_free(p_);
            p_ = nullptr;
            n_ = 0;
            return;
        }
        ok_ = true;
    }

    ~LockedBuffer() { release(); }

    LockedBuffer(const LockedBuffer&) = delete;
    LockedBuffer& operator=(const LockedBuffer&) = delete;

    LockedBuffer(LockedBuffer&& o) noexcept : p_(o.p_), n_(o.n_), ok_(o.ok_) {
        o.p_ = nullptr;
        o.n_ = 0;
        o.ok_ = false;
    }
    LockedBuffer& operator=(LockedBuffer&& o) noexcept {
        if (this != &o) {
            release();
            p_ = o.p_;
            n_ = o.n_;
            ok_ = o.ok_;
            o.p_ = nullptr;
            o.n_ = 0;
            o.ok_ = false;
        }
        return *this;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] std::byte* data() noexcept { return static_cast<std::byte*>(p_); }
    [[nodiscard]] const std::byte* data() const noexcept { return static_cast<const std::byte*>(p_); }
    [[nodiscard]] std::span<std::byte> span() noexcept { return std::span<std::byte>(data(), n_); }
    [[nodiscard]] std::span<const std::byte> span() const noexcept { return std::span<const std::byte>(data(), n_); }

private:
    void release() noexcept {
        if (p_ == nullptr) return;
        if (ok_) {
            ::sodium_munlock(p_, n_);  // zeroes the range, then unlocks
        } else {
            secure_wipe(span());  // allocated but never locked
        }
        ::sodium_free(p_);
        p_ = nullptr;
        n_ = 0;
        ok_ = false;
    }

    void* p_ = nullptr;
    std::size_t n_ = 0;
    bool ok_ = false;
};

// Scrubs a stack temporary that holds key material on EVERY exit from its scope (normal return, early return, and
// exception unwinding), instead of relying on one wipe per explicit exit. Does not own the bytes.
class ScopedWipe {
public:
    explicit ScopedWipe(std::span<std::byte> s) noexcept : s_(s) {}
    ~ScopedWipe() { secure_wipe(s_); }
    ScopedWipe(const ScopedWipe&) = delete;
    ScopedWipe& operator=(const ScopedWipe&) = delete;

private:
    std::span<std::byte> s_;
};

}  // namespace quark::adapters
