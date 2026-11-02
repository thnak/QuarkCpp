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
// The scan instruments the global operator new/delete (tests/adapters/heap_probe.cpp, its own translation unit): every
// allocation is recorded and every block is snapshotted into a bounded quarantine at free time, so a leak that is freed
// before the scan is still visible. It is single-threaded by construction (the test starts no threads) and is compiled
// OUT under ASan/TSan, where the sanitizer owns operator new; the functional and lock-refusal checks still run there.
//
// Implements 020-Security §"Pinned-key mode (Noise IK, #40)"; issue #64.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "heap_probe.hpp"
#include "pinned_test_util.hpp"

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
        check(heap_probe::scan_live(marker) > 0,
              "T0 POSITIVE CONTROL: the scan finds the marker in an ordinary heap block", ok);
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

    live_hits = heap_probe::scan_live(marker);
    freed_hits = heap_probe::scan_freed(marker);
    seed_live = heap_probe::scan_live(seed);
    seed_freed = heap_probe::scan_freed(seed);

    std::printf("key scan: x25519 hits live=%zu freed=%zu | ed25519 seed hits live=%zu freed=%zu\n", live_hits,
                freed_hits, seed_live, seed_freed);
    if (heap_probe::enabled()) {
        check(heap_probe::complete(), "T1: the scan itself was complete (no allocation table / quarantine overflow)",
              ok);
    } else {
        std::printf("key scan: instrumented allocator skipped under a sanitizer; the marker scan did not run\n");
    }

#if defined(QUARK_PINNED_KEY_COPIES_CONTROL)
    // WILL_FAIL contract: this binary must exit NON-ZERO when it demonstrates the leak, and zero when it does not (a
    // control that stopped leaking is a broken control, and WILL_FAIL then reports the zero exit as a failure).
    check(seed_live + seed_freed == 0, "T2 CONTROL: the Ed25519 seed is still absent from ordinary heap", ok);
    if (!heap_probe::enabled()) {
        std::printf("control: scan skipped under a sanitizer; exiting non-zero by construction\n");
        return ok ? 1 : 0;
    }
    {
        const bool leaked = (live_hits + freed_hits) > 0;
        std::printf("control: by-value KeyPair leak %s\n",
                    leaked ? "OBSERVED (expected; this binary exits 1)" : "MISSING (the control is broken)");
        return (leaked && ok) ? 1 : 0;
    }
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
