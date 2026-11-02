// #64 step 2 (AeroCoWorker ADR-047 D3, ADR-068 finding 8): prove that the pinned path keeps the static secret out of
// ordinary heap. The identity's Ed25519 seed and its derived X25519 static secret are the only copies, they live in
// LockedBuffer (sodium_malloc + a CHECKED mlock), and the handshake BORROWS them instead of copying.
//
//   T0 POSITIVE CONTROL: the scan itself finds the marker in an ordinary heap block (so a clean T1 means something).
//   T1 after handshakes and a rekey, with every engine and identity destroyed, no live-or-freed ordinary heap block
//      outside locked memory contains the static secret (neither the X25519 secret nor the Ed25519 seed).
//   T2 CONTROL (built with -DQUARK_PINNED_KEY_COPIES_CONTROL): PinnedOptions::test_copy_static_key restores the pre-fix
//      by-value KeyPair heap copy; the scan FINDS the marker. The target is registered WILL_FAIL, so a control that
//      stops seeing the leak fails the build.
//   T3 a refused mlock is refused: from_secret_key returns nullopt, generate() terminates instead of spinning, and the
//      control (lock allowed) still succeeds.
//
// The scan instruments the global operator new/delete: every allocation is recorded and every block is snapshotted into
// a bounded quarantine at free time, so a leak that is freed before the scan is still visible. It is single-threaded by
// construction (the test starts no threads) and is compiled OUT under ASan/TSan, where the sanitizer owns operator new;
// the functional and lock-refusal checks still run there.
//
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)"; issue #64.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

#include "pinned_test_util.hpp"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  define QUARK_KEY_SCAN 0
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define QUARK_KEY_SCAN 0
#  else
#    define QUARK_KEY_SCAN 1
#  endif
#else
#  define QUARK_KEY_SCAN 1
#endif

#if QUARK_KEY_SCAN
#  if defined(_MSC_VER)
#    include <malloc.h>
#  endif

namespace {

// ---- instrumented allocator -----------------------------------------------------------------------
struct Slot {
    void* p;
    std::size_t n;
};
void* const kTombstone = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
constexpr std::size_t kSlotCount = 1u << 17;
Slot g_slots[kSlotCount];  // zero-initialised statically: recording never allocates
bool g_table_overflow = false;

constexpr std::size_t kFreedMax = 1u << 16;
constexpr std::size_t kQuarantineCap = 32u << 20;
struct Freed {
    std::size_t off;
    std::size_t len;
};
Freed g_freed[kFreedMax];
std::size_t g_freed_n = 0;
std::byte g_quarantine[kQuarantineCap];
std::size_t g_quarantine_used = 0;
bool g_quarantine_overflow = false;

[[nodiscard]] std::size_t slot_of(void* p) noexcept {
    auto x = reinterpret_cast<std::uintptr_t>(p);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x) & (kSlotCount - 1);
}

void record(void* p, std::size_t n) noexcept {
    const std::size_t start = slot_of(p);
    std::size_t first_tomb = kSlotCount;
    for (std::size_t k = 0; k < kSlotCount; ++k) {
        Slot& s = g_slots[(start + k) & (kSlotCount - 1)];
        if (s.p == p) {
            s.n = n;
            return;
        }
        if (s.p == kTombstone && first_tomb == kSlotCount) first_tomb = (start + k) & (kSlotCount - 1);
        if (s.p == nullptr) {
            Slot& put = first_tomb == kSlotCount ? s : g_slots[first_tomb];
            put.p = p;
            put.n = n;
            return;
        }
    }
    g_table_overflow = true;
}

// Removes `p` and returns its size (0 when unknown).
[[nodiscard]] std::size_t take(void* p) noexcept {
    const std::size_t start = slot_of(p);
    for (std::size_t k = 0; k < kSlotCount; ++k) {
        Slot& s = g_slots[(start + k) & (kSlotCount - 1)];
        if (s.p == nullptr) return 0;
        if (s.p == p) {
            const std::size_t n = s.n;
            s.p = kTombstone;
            s.n = 0;
            return n;
        }
    }
    return 0;
}

void quarantine(void* p) noexcept {
    const std::size_t n = take(p);
    if (n == 0) return;
    if (g_freed_n == kFreedMax || n > kQuarantineCap - g_quarantine_used) {
        g_quarantine_overflow = true;
        return;
    }
    std::memcpy(g_quarantine + g_quarantine_used, p, n);
    g_freed[g_freed_n].off = g_quarantine_used;
    g_freed[g_freed_n].len = n;
    ++g_freed_n;
    g_quarantine_used += n;
}

[[nodiscard]] std::size_t count_in(std::span<const std::byte> hay, std::span<const std::byte> needle) noexcept {
    if (needle.empty() || hay.size() < needle.size()) return 0;
    std::size_t hits = 0;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) ++hits;
    return hits;
}

[[nodiscard]] std::size_t scan_live(std::span<const std::byte> needle) noexcept {
    std::size_t hits = 0;
    for (const Slot& s : g_slots)
        if (s.p != nullptr && s.p != kTombstone)
            hits += count_in(std::span<const std::byte>(static_cast<const std::byte*>(s.p), s.n), needle);
    return hits;
}

[[nodiscard]] std::size_t scan_freed(std::span<const std::byte> needle) noexcept {
    std::size_t hits = 0;
    for (std::size_t i = 0; i < g_freed_n; ++i)
        hits += count_in(std::span<const std::byte>(g_quarantine + g_freed[i].off, g_freed[i].len), needle);
    return hits;
}

}  // namespace

// ---- global operator new/delete replacement -------------------------------------------------------
void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    record(p, n == 0 ? 1 : n);
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
    if (p == nullptr) return;
    quarantine(p);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p != nullptr) record(p, n == 0 ? 1 : n);
    return p;
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

void* operator new(std::size_t n, std::align_val_t a) {
    const std::size_t al = static_cast<std::size_t>(a);
    void* p = nullptr;
#  if defined(_MSC_VER)
    p = _aligned_malloc(n == 0 ? 1 : n, al);
#  else
    const std::size_t sz = ((n == 0 ? 1 : n) + al - 1) / al * al;
    if (posix_memalign(&p, al, sz) != 0) p = nullptr;
#  endif
    if (p == nullptr) throw std::bad_alloc();
    record(p, n == 0 ? 1 : n);
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
    if (p == nullptr) return;
    quarantine(p);
#  if defined(_MSC_VER)
    _aligned_free(p);
#  else
    std::free(p);
#  endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    void* p = nullptr;
    try {
        p = ::operator new(n, a);
    } catch (...) {
        p = nullptr;
    }
    return p;
}
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p, a); }

#else  // QUARK_KEY_SCAN
namespace {
[[nodiscard]] std::size_t scan_live(std::span<const std::byte>) noexcept { return 0; }
[[nodiscard]] std::size_t scan_freed(std::span<const std::byte>) noexcept { return 0; }
}  // namespace
#endif  // QUARK_KEY_SCAN

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {
const ClusterId kCluster{6464};
}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;

    // A REAL identity: its derived keys are the markers. Generate first — a libsodium Ed25519 secret key is
    // seed||public and crypto_sign_ed25519_sk_to_pk copies the cached public half, so an arbitrary seed cannot be spliced
    // in without making the identity inconsistent (which from_secret_key then refuses, correctly).
    PinnedIdentity a_identity = PinnedIdentity::generate();
    std::array<std::byte, PinnedIdentity::kSecretKeyBytes> sk{};
    std::memcpy(sk.data(), a_identity.secret_key().data(), sk.size());
    std::array<std::byte, 32> seed{};
    std::memcpy(seed.data(), sk.data(), seed.size());
    std::array<std::byte, 32> marker{};
    std::memcpy(marker.data(), a_identity.noise_static().sk.data(), marker.size());

    // ---- T0 POSITIVE CONTROL: the scan sees an ordinary heap copy ------------------------------------------------
    {
        std::vector<std::byte> probe(marker.begin(), marker.end());
        check(scan_live(marker) > 0, "T0 POSITIVE CONTROL: the scan finds the marker in an ordinary heap block", ok);
        std::fill(probe.begin(), probe.end(), std::byte{0});  // do not quarantine the marker
    }

    // ---- T1: handshakes + a rekey, then the static secret is in no ordinary heap block ---------------------------
    std::size_t live_hits = 0, freed_hits = 0, seed_live = 0, seed_freed = 0;
    {
        PinnedIdentity b_identity = PinnedIdentity::generate();

        PinnedOptions opt;
#if defined(QUARK_PINNED_KEY_COPIES_CONTROL)
        opt.test_copy_static_key = true;  // the pre-fix by-value KeyPair heap copy (control only)
#endif
        Node a(std::move(a_identity), opt);
        Node b(std::move(b_identity), opt);
        pair_up(a, b, kCluster);

        const auto ns = a.factory->identity().noise_static();
        std::memcpy(marker.data(), ns.sk.data(), marker.size());
        check(std::equal(marker.begin(), marker.end(), ns.sk.begin()),
              "T1: the X25519 static secret is reachable through the borrowed view", ok);

        // One handshake, then a second one (the rekey shape SecureTransport uses): both directions, both roles.
        for (int round = 0; round < 2; ++round) {
            const Outcome o = run_pair(*a.engine(true, b.id, kCluster), *b.engine(false, a.id, kCluster));
            check(o.both_done(), round == 0 ? "T1: the handshake completes" : "T1: the rekey completes", ok);
        }
    }  // engines, identities and sessions destroyed here

    live_hits = scan_live(marker);
    freed_hits = scan_freed(marker);
    seed_live = scan_live(seed);
    seed_freed = scan_freed(seed);

    std::printf("key scan: x25519 hits live=%zu freed=%zu | ed25519 seed hits live=%zu freed=%zu\n", live_hits,
                freed_hits, seed_live, seed_freed);
#if QUARK_KEY_SCAN
    check(!g_table_overflow && !g_quarantine_overflow,
          "T1: the scan itself was complete (no allocation table / quarantine overflow)", ok);
#else
    std::printf("key scan: instrumented allocator skipped under a sanitizer; the marker scan did not run\n");
#endif

#if defined(QUARK_PINNED_KEY_COPIES_CONTROL)
    // WILL_FAIL contract: this binary must exit NON-ZERO when it demonstrates the leak, and zero when it does not (a
    // control that stopped leaking is a broken control, and WILL_FAIL then reports the zero exit as a failure).
    check(seed_live + seed_freed == 0, "T2 CONTROL: the Ed25519 seed is still absent from ordinary heap", ok);
#  if !QUARK_KEY_SCAN
    std::printf("control: scan skipped under a sanitizer; exiting non-zero by construction\n");
    return ok ? 1 : 0;
#  else
    {
        const bool leaked = (live_hits + freed_hits) > 0;
        std::printf("control: by-value KeyPair leak %s\n",
                    leaked ? "OBSERVED (expected; this binary exits 1)" : "MISSING (the control is broken)");
        return (leaked && ok) ? 1 : 0;
    }
#  endif
#else
    check(live_hits + freed_hits == 0,
          "T1: no live or freed ordinary heap block holds the X25519 static secret after teardown", ok);
    check(seed_live + seed_freed == 0, "T1: no live or freed ordinary heap block holds the Ed25519 seed", ok);
#endif

    // ---- T3: a refused lock is refused (fail closed) ------------------------------------------------------------
    {
        int (*const saved)(void*, std::size_t) = quark::adapters::detail::mlock_probe_for_test;
        quark::adapters::detail::mlock_probe_for_test = [](void*, std::size_t) -> int { return -1; };

        const auto refused = PinnedIdentity::from_secret_key(
            std::span<const std::byte, PinnedIdentity::kSecretKeyBytes>(sk), PinnedRole::Member);
        check(!refused.has_value(), "T3: an identity whose secret cannot be locked is REFUSED (fail closed)", ok);

        bool threw = false;
        try {
            (void)PinnedIdentity::generate();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, "T3: generate() terminates (throws) instead of spinning when the lock is refused", ok);

        quark::adapters::detail::mlock_probe_for_test = saved;
        const auto allowed = PinnedIdentity::from_secret_key(
            std::span<const std::byte, PinnedIdentity::kSecretKeyBytes>(sk), PinnedRole::Member);
        check(allowed.has_value(), "T3 CONTROL: with the lock allowed the same identity is accepted", ok);
    }

    std::printf(ok ? "pinned_key_copies_check: OK\n" : "pinned_key_copies_check: FAILED\n");
    return ok ? 0 : 1;
}
