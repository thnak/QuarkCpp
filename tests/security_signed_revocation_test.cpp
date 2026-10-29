// Tests ADR-067 D2 — issuer-authenticated (anchor-signed) revocation: the record format
// (signed_revocation.hpp), RevocationRegistry's signed mode, the SWIM signed-revocation section
// (cluster.hpp), and the Strict startup check (security.hpp). Std-only: a MockVerifier stands in for the
// trust anchor with a keyed toy signature; real ECDSA P-256 is in tests/adapters/mbedtls_handshake_check.cpp.
//
// Load-bearing properties (each with a positive control):
//   C1  a member that is not the anchor cannot revoke a third party: unsigned gossip (also when learned
//       before signed mode was enabled), a wrong signer, a tampered record, a wrong cluster and an unknown
//       root are all refused;
//   C7  a signed revocation is never dropped silently: no verifier ⇒ revoke() errors; full cap ⇒ errors /
//       counted; it survives a trust-root ROTATION (re-signed copy replaces the old one, never the
//       reverse; a node joining after the old root is gone still learns it);
//   bounds: expired records are refused before verification (no replay after a prune); gossip
//   verification is budgeted per merge and by a token bucket; a held fingerprint is not re-verified;
//   the SWIM section is appended after the version trailer, bounded, and converges between nodes.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/engine_config.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/revocation_registry.hpp"
#include "quark/core/security.hpp"
#include "quark/core/signed_revocation.hpp"
#include "quark/core/tls_identity.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

constexpr std::uint64_t kNow = 1'800'000'000;      // the caller's wall clock (unix seconds)
constexpr std::uint64_t kLater = 2'000'000'000;    // a not_after comfortably in the future

[[nodiscard]] Fingerprint fp_n(std::uint8_t n) {
    Fingerprint fp{};
    fp[0] = static_cast<std::byte>(n);
    fp[31] = static_cast<std::byte>(0xA5);
    return fp;
}
[[nodiscard]] AnchorKeyId anchor_n(std::uint8_t n) {
    AnchorKeyId id{};
    id[0] = static_cast<std::byte>(n);
    return id;
}

// A keyed toy signature: 8 bytes of an FNV-1a fold of (secret, message). Stands in for ECDSA.
[[nodiscard]] std::vector<std::byte> toy_sign(std::uint64_t secret, std::span<const std::byte> msg) {
    std::uint64_t h = 1469598103934665603ULL ^ secret;
    for (std::byte b : msg) {
        h ^= static_cast<unsigned char>(b);
        h *= 1099511628211ULL;
    }
    std::vector<std::byte> sig(8);
    for (int i = 0; i < 8; ++i) sig[static_cast<std::size_t>(i)] = static_cast<std::byte>((h >> (8 * i)) & 0xFF);
    return sig;
}

// Roots: anchor id byte → secret. `current` lists the roots of the CURRENT trust set (the rest are the
// overlap-window previous ones). Counts signature checks so tests can assert work was (not) spent.
class MockVerifier final : public TrustAnchorVerifier {
public:
    std::unordered_map<std::uint8_t, std::uint64_t> secrets;
    std::unordered_set<std::uint8_t> current;
    mutable std::atomic<int> calls{0};
    [[nodiscard]] bool verify(const AnchorKeyId& id, std::span<const std::byte> msg,
                              std::span<const std::byte> sig) const override {
        calls.fetch_add(1, std::memory_order_relaxed);
        const auto it = secrets.find(static_cast<std::uint8_t>(id[0]));
        if (it == secrets.end()) return false;
        const auto want = toy_sign(it->second, msg);
        return sig.size() == want.size() && std::equal(sig.begin(), sig.end(), want.begin());
    }
    [[nodiscard]] bool is_current_root(const AnchorKeyId& id) const override {
        return current.contains(static_cast<std::uint8_t>(id[0])) && secrets.contains(static_cast<std::uint8_t>(id[0]));
    }
};

constexpr std::uint64_t kAnchorSecret = 0xA11CE;  // root 1
constexpr std::uint64_t kNewRootSecret = 0x5EC0;  // root 2 (rotated in)
constexpr std::uint64_t kRogueSecret = 0xBAD;

[[nodiscard]] std::shared_ptr<MockVerifier> make_verifier() {
    auto v = std::make_shared<MockVerifier>();
    v->secrets[1] = kAnchorSecret;
    v->current.insert(1);
    return v;
}

[[nodiscard]] SignedRevocation make_rec(ClusterId cluster, std::uint8_t fp, std::uint64_t not_after = kLater,
                                        std::uint8_t anchor = 1, std::uint64_t secret = kAnchorSecret) {
    SignedRevocation r;
    r.cluster = cluster;
    r.fingerprint = fp_n(fp);
    r.not_after = not_after;
    r.issued_at = 1'700'000'000;
    r.anchor_key_id = anchor_n(anchor);
    r.signature = toy_sign(secret, revocation_signed_bytes(r));
    return r;
}

[[nodiscard]] std::vector<std::vector<std::byte>> blobs_of(std::initializer_list<SignedRevocation> recs) {
    std::vector<std::vector<std::byte>> out;
    for (const SignedRevocation& r : recs) out.push_back(encode_signed_revocation(r));
    return out;
}

// Generous budget so tests that are not ABOUT the budget never hit it.
[[nodiscard]] SignedRevocationLimits roomy(std::size_t max_signed = 64) {
    SignedRevocationLimits l;
    l.max_signed = max_signed;
    l.max_verifies_per_merge = 64;
    l.verify_burst = 1000;
    l.verifies_per_second = 1000;
    return l;
}

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{4242};

    // ============================================================================================
    // A. Record format.
    // ============================================================================================
    {
        const SignedRevocation r = make_rec(cluster, 7);
        const auto signed_bytes = revocation_signed_bytes(r);
        check(signed_bytes.size() == 104, "the signed message is exactly 104 bytes", ok);
        check(signed_bytes[0] == std::byte{'Q'} && signed_bytes[0] != std::byte{0x30},
              "the signed message starts with the ASCII domain prefix, never a DER SEQUENCE", ok);
        const auto wire = encode_signed_revocation(r);
        SignedRevocation back;
        check(decode_signed_revocation(wire, back), "encode/decode round-trips", ok);
        check(back.cluster == r.cluster && back.fingerprint == r.fingerprint && back.not_after == r.not_after &&
                  back.issued_at == r.issued_at && back.anchor_key_id == r.anchor_key_id &&
                  back.signature == r.signature,
              "every field survives the round trip", ok);

        auto bad = wire;
        bad[3] = std::byte{'X'};
        check(!decode_signed_revocation(bad, back), "a wrong domain prefix is refused", ok);
        bad = wire;
        bad.push_back(std::byte{0});
        check(!decode_signed_revocation(bad, back), "a trailing byte is refused (exact length)", ok);
        bad = wire;
        bad.pop_back();
        check(!decode_signed_revocation(bad, back), "a truncated signature is refused", ok);
        bad = wire;
        bad[104] = std::byte{0};
        bad.resize(105);
        check(!decode_signed_revocation(bad, back), "a zero-length signature is refused", ok);
        SignedRevocation big = r;
        big.signature.assign(73, std::byte{1});
        check(!decode_signed_revocation(encode_signed_revocation(big), back), "a 73-byte signature is refused", ok);
    }

    // ============================================================================================
    // B. Without a verifier: legacy unsigned behaviour, and revoke(signed) is an ERROR, not a silent no-op.
    // ============================================================================================
    {
        RevocationRegistry reg;
        const auto res = reg.revoke(make_rec(cluster, 1), kNow);
        check(!res && res.error().code == errc::unavailable, "revoke(signed) without a verifier returns unavailable", ok);
        check(!reg.is_revoked(fp_n(1)), "and revokes nothing", ok);
        check(reg.merge({fp_n(9)}) && reg.is_revoked(fp_n(9)), "control: unsigned gossip still merges in legacy mode", ok);
        check(reg.sample(8).size() == 1, "control: legacy mode gossips unsigned fingerprints", ok);
    }

    // ============================================================================================
    // C. Signed mode: what is accepted, what is refused, the cap.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        RevocationRegistry reg;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy(3))), "signed mode enables", ok);
        check(reg.signed_mode(), "signed mode is on", ok);

        check(static_cast<bool>(reg.revoke(make_rec(cluster, 1), kNow)), "an anchor-signed record is accepted", ok);
        check(reg.is_revoked(fp_n(1)) && reg.snapshot()->contains(fp_n(1)),
              "and enforced (registry and the handshake/sweep snapshot)", ok);

        // C1: every way a non-anchor could try to revoke a third party.
        const auto refused = [&](const SignedRevocation& rec, const char* what) {
            const auto res = reg.revoke(rec, kNow);
            check(!res && res.error().code == errc::validation && !reg.is_revoked(rec.fingerprint), what, ok);
        };
        refused(make_rec(ClusterId{1}, 20), "C1: a record for another cluster is refused");
        refused(make_rec(cluster, 21, kLater, /*anchor=*/9), "C1: an unknown root is refused");
        refused(make_rec(cluster, 22, kLater, 1, kRogueSecret), "C1: a wrong signer claiming the anchor is refused");
        {
            SignedRevocation t = make_rec(cluster, 23);
            t.fingerprint = fp_n(24);  // retarget a valid record at a third party
            refused(t, "C1: a tampered (re-targeted) record is refused");
            SignedRevocation e = make_rec(cluster, 25);
            e.not_after += 1;  // extend its lifetime
            refused(e, "C1: a tampered not_after is refused");
        }
        check(reg.signed_rejected() == 5, "every refused record is counted", ok);

        const auto rejected_before = reg.rejected();
        check(!reg.merge({fp_n(30)}) && !reg.is_revoked(fp_n(30)), "C1: UNSIGNED gossip is refused in signed mode", ok);
        check(reg.rejected() == rejected_before + 1, "and counted", ok);
        reg.revoke_locally(fp_n(31));
        check(reg.is_revoked(fp_n(31)) && reg.snapshot()->contains(fp_n(31)),
              "a local operator revocation still applies locally (registry and enforced snapshot)", ok);
        check(reg.sample(8).empty(), "but never gossips unsigned in signed mode", ok);

        // Identity is the fingerprint: a second record for a held fingerprint is not re-verified.
        const int calls_before = verifier->calls.load();
        SignedRevocation dup = make_rec(cluster, 1);
        dup.signature.assign(8, std::byte{0x77});  // a different (here: garbage) encoding, same root
        check(static_cast<bool>(reg.revoke(dup, kNow)), "a held fingerprint is idempotent", ok);
        check(verifier->calls.load() == calls_before, "and is never re-verified", ok);

        // merge_signed: new + malformed + bad signature + duplicate in one batch.
        std::vector<std::vector<std::byte>> batch = blobs_of({make_rec(cluster, 2), make_rec(cluster, 3, kLater, 1, kRogueSecret),
                                                              make_rec(cluster, 1), make_rec(cluster, 2)});
        batch.insert(batch.begin() + 1, std::vector<std::byte>(10, std::byte{1}));  // malformed
        const auto rejected_signed = reg.signed_rejected();
        const int calls_mid = verifier->calls.load();
        check(reg.merge_signed(batch, kNow), "a gossip batch with one valid new record reports a change", ok);
        check(reg.is_revoked(fp_n(2)) && !reg.is_revoked(fp_n(3)), "only the anchor-signed record is applied", ok);
        check(reg.signed_rejected() == rejected_signed + 2, "malformed and forged records are counted", ok);
        check(verifier->calls.load() == calls_mid + 2,
              "held and in-batch duplicate fingerprints are not verified (2 checks for 2 fresh records)", ok);
        check(!reg.merge_signed(blobs_of({make_rec(cluster, 1)}), kNow), "replay is idempotent (no change)", ok);

        // C7: the signed cap refuses LOUDLY, and gossip does no signature work against a full cap.
        check(static_cast<bool>(reg.revoke(make_rec(cluster, 4, kNow + 100), kNow)), "third signed record fills the cap (3)", ok);
        const auto over = reg.revoke(make_rec(cluster, 5), kNow);
        check(!over && over.error().code == errc::overloaded, "C7: a full signed cap makes revoke() fail loudly", ok);
        const int calls_full = verifier->calls.load();
        check(!reg.merge_signed(blobs_of({make_rec(cluster, 6)}), kNow), "a full cap refuses gossip too", ok);
        check(reg.signed_cap_refused() == 1 && verifier->calls.load() == calls_full,
              "and counts it without spending a signature check", ok);
        check(reg.signed_revocations().size() == 3, "the held set is exported for re-signing on rotation", ok);

        // Prune: record 4 has not_after = kNow + 100; the default skew is 300 s.
        check(reg.prune_expired(kNow + 100 + 300) == 0, "not pruned within the skew bound", ok);
        reg.revoke_locally(fp_n(4));  // also revoked locally: must survive pruning of the signed entry
        check(reg.prune_expired(kNow + 100 + 301) == 1, "pruned once not_after + skew has passed", ok);
        check(reg.is_revoked(fp_n(4)) && reg.snapshot()->contains(fp_n(4)),
              "a fingerprint also revoked locally stays revoked (and enforced) after the prune", ok);
        check(reg.snapshot()->size() == reg.size() && reg.snapshot()->contains(fp_n(1)),
              "the enforced snapshot is the union of signed and unsigned entries", ok);
        check(reg.signed_size() == 2, "the pruned slot is free again", ok);
        check(static_cast<bool>(reg.revoke(make_rec(cluster, 5), kNow)), "the cap admits a new record after a prune", ok);
    }

    // ============================================================================================
    // M3. Expired records are refused BEFORE verification — a pruned record cannot be replayed back in.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        RevocationRegistry reg;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy(8))), "enable (expiry)", ok);
        const SignedRevocation short_lived = make_rec(cluster, 40, kNow + 10);
        check(static_cast<bool>(reg.revoke(short_lived, kNow)), "a short-lived record is accepted while valid", ok);
        const std::uint64_t much_later = kNow + 10 + 301;
        check(reg.prune_expired(much_later) == 1, "and pruned once expired", ok);
        const int calls_before = verifier->calls.load();
        check(!reg.merge_signed(blobs_of({short_lived}), much_later) && !reg.is_revoked(short_lived.fingerprint),
              "M3: replaying the pruned record over gossip does not bring it back", ok);
        check(reg.signed_expired() == 1 && verifier->calls.load() == calls_before,
              "and it is counted as expired without a signature check", ok);
        const auto res = reg.revoke(short_lived, much_later);
        check(!res && res.error().code == errc::validation, "the application path refuses an expired record too", ok);
    }
    {
        // A prune that runs WHILE a record is being verified (the lock is released) must not be undone: the
        // apply step re-checks expiry against the latest prune. The verifier itself triggers the prune.
        struct PruningVerifier final : TrustAnchorVerifier {
            std::shared_ptr<MockVerifier> inner = make_verifier();
            RevocationRegistry* reg = nullptr;
            std::uint64_t prune_at = 0;
            bool verify(const AnchorKeyId& id, std::span<const std::byte> m, std::span<const std::byte> s) const override {
                (void)reg->prune_expired(prune_at);
                return inner->verify(id, m, s);
            }
            bool is_current_root(const AnchorKeyId& id) const override { return inner->is_current_root(id); }
        };
        auto verifier = std::make_shared<PruningVerifier>();
        RevocationRegistry reg;
        verifier->reg = &reg;
        verifier->prune_at = kNow + 10 + 301;  // the record below is expired at this reading
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy())), "enable (prune race)", ok);
        (void)reg.merge_signed(blobs_of({make_rec(cluster, 41, kNow + 10)}), kNow);
        check(!reg.is_revoked(fp_n(41)), "M3: a prune during verification is not undone by the apply step", ok);
        check(static_cast<bool>(reg.revoke(make_rec(cluster, 42, kLater), kNow)) && reg.is_revoked(fp_n(42)),
              "control: an unexpired record is applied across the same prune", ok);
    }
    {
        // A copy that lands WHILE another is being verified must not be overwritten by it: the apply step
        // only replaces the exact held copy the rotation decision was made against. Here gossip carries an
        // old-root copy; mid-verify the application applies the current-root copy of the same fingerprint.
        struct RacingVerifier final : TrustAnchorVerifier {
            std::shared_ptr<MockVerifier> inner = make_verifier();
            RevocationRegistry* reg = nullptr;
            SignedRevocation concurrent{};
            mutable bool fired = false;
            bool verify(const AnchorKeyId& id, std::span<const std::byte> m, std::span<const std::byte> s) const override {
                if (!fired) {
                    fired = true;
                    (void)reg->revoke(concurrent, kNow);
                }
                return inner->verify(id, m, s);
            }
            bool is_current_root(const AnchorKeyId& id) const override { return inner->is_current_root(id); }
        };
        auto verifier = std::make_shared<RacingVerifier>();
        verifier->inner->secrets[2] = kNewRootSecret;
        verifier->inner->current = {2};  // root 1 is the overlap-window previous root
        RevocationRegistry reg;
        verifier->reg = &reg;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy())), "enable (apply race)", ok);
        verifier->concurrent = make_rec(cluster, 43, kLater, 2, kNewRootSecret);
        (void)reg.merge_signed(blobs_of({make_rec(cluster, 43)}), kNow);  // the old-root copy
        check(reg.signed_size() == 1 && reg.signed_revocations()[0].anchor_key_id == anchor_n(2),
              "M1: a current-root copy applied mid-verify is not overwritten by the old-root copy", ok);
    }

    // ============================================================================================
    // M4. enable_signed: one-way, non-null, and it DROPS unsigned entries learned from gossip earlier.
    // ============================================================================================
    {
        RevocationRegistry reg;
        check(reg.merge({fp_n(50)}), "legacy mode: a member gossips a third-party revocation", ok);
        reg.revoke_locally(fp_n(51));
        auto verifier = make_verifier();
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy())), "enable after unsigned gossip", ok);
        check(!reg.is_revoked(fp_n(50)) && !reg.snapshot()->contains(fp_n(50)),
              "M4: the gossip-learned unsigned revocation is dropped on enable (C1 does not depend on startup order)", ok);
        check(reg.is_revoked(fp_n(51)) && reg.snapshot()->contains(fp_n(51)), "an operator-local revocation is kept", ok);
        const auto again = reg.enable_signed(make_verifier(), ClusterId{1}, roomy());
        check(!again && again.error().code == errc::validation, "a second enable_signed is refused (no verifier swap)", ok);
        RevocationRegistry fresh;
        const auto null_v = fresh.enable_signed(nullptr, cluster);
        check(!null_v && null_v.error().code == errc::validation && !fresh.signed_mode(),
              "a null verifier is refused and leaves the registry in legacy mode", ok);
        SignedRevocationLimits zero = roomy();
        zero.max_verifies_per_merge = 0;
        const auto zero_res = fresh.enable_signed(make_verifier(), cluster, zero);
        check(!zero_res && zero_res.error().code == errc::validation && !fresh.signed_mode(),
              "a zero limit (it would silently disable gossip verification) is refused", ok);
    }

    // ============================================================================================
    // M2. Gossip verification is budgeted: per merge, and by a token bucket across merges.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        RevocationRegistry reg;
        SignedRevocationLimits l;
        l.max_signed = 1000;
        l.max_verifies_per_merge = 8;
        l.verify_burst = 10;
        l.verifies_per_second = 2;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, l)), "enable (budget)", ok);
        std::vector<std::vector<std::byte>> flood;  // 20 forged records, each a fresh fingerprint
        for (std::uint8_t i = 0; i < 20; ++i)
            flood.push_back(encode_signed_revocation(make_rec(cluster, static_cast<std::uint8_t>(100 + i), kLater, 1, kRogueSecret)));
        const int c0 = verifier->calls.load();
        (void)reg.merge_signed(flood, kNow);
        check(verifier->calls.load() - c0 == 8, "M2: one merge spends at most max_verifies_per_merge checks", ok);
        check(reg.signed_deferred() == 12, "the rest are dropped unverified and counted", ok);
        (void)reg.merge_signed(flood, kNow);
        check(verifier->calls.load() - c0 == 10, "M2: the token bucket (burst 10) caps checks across merges", ok);
        (void)reg.merge_signed(flood, kNow + 1);
        check(verifier->calls.load() - c0 == 12, "M2: and refills at verifies_per_second (2/s)", ok);
        // Positive control: a genuine record still gets through once the bucket has tokens.
        (void)reg.merge_signed(blobs_of({make_rec(cluster, 60)}), kNow + 10);
        check(reg.is_revoked(fp_n(60)), "control: a genuine record is applied once budget is available", ok);

        // The caller's clock jumps far ahead, then steps back: the bucket must keep refilling from the new
        // reading, not stall until the clock passes the old high-water mark again.
        (void)reg.merge_signed(flood, kNow + 1'000'000);  // drains the bucket at the far-future reading
        const int c1 = verifier->calls.load();
        (void)reg.merge_signed(flood, kNow + 20);  // step back: rebase, refills nothing
        (void)reg.merge_signed(flood, kNow + 25);  // 5 s later on the corrected clock
        check(verifier->calls.load() - c1 == 2 + 8, "M2: after a clock step back (2 tokens left, then a 5 s refill) the bucket refills again (no stall)", ok);
    }

    // ============================================================================================
    // M2b. Every held record is gossiped: sample_signed walks the set round-robin, so a set larger than
    //      one frame's share still reaches peers in full.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        RevocationRegistry node_a, node_b;
        check(static_cast<bool>(node_a.enable_signed(verifier, cluster, roomy())) &&
                  static_cast<bool>(node_b.enable_signed(verifier, cluster, roomy())),
              "enable (sampling)", ok);
        for (std::uint8_t i = 0; i < 20; ++i)
            check(static_cast<bool>(node_a.revoke(make_rec(cluster, static_cast<std::uint8_t>(150 + i)), kNow)),
                  "revoke 20 records", ok);
        for (int round = 0; round < 3; ++round) (void)node_b.merge_signed(node_a.sample_signed(8), kNow);
        check(node_b.signed_size() == 20, "three 8-record samples carry all 20 records (round-robin, none starved)", ok);
        for (std::uint8_t i = 0; i < 5; ++i)
            (void)node_a.revoke(make_rec(cluster, static_cast<std::uint8_t>(180 + i), kNow + 10), kNow);
        check(node_a.prune_expired(kNow + 400) == 5, "five short-lived records pruned", ok);
        std::size_t sampled = 0;
        for (int round = 0; round < 5; ++round) sampled += node_a.sample_signed(8).size();
        check(sampled == 40 && node_a.signed_size() == 20, "sampling after a prune walks only the held records", ok);
    }

    // ============================================================================================
    // M2c. The verifier is never called with the registry lock held (it has its own lock; nesting would
    //      stall every is_revoked/snapshot reader behind a slow verify). A verifier that calls back into
    //      the registry would deadlock if it were.
    // ============================================================================================
    {
        struct ReentrantVerifier final : TrustAnchorVerifier {
            std::shared_ptr<MockVerifier> inner = make_verifier();
            RevocationRegistry* reg = nullptr;
            mutable std::atomic<int> reentries{0};
            bool verify(const AnchorKeyId& id, std::span<const std::byte> m, std::span<const std::byte> s) const override {
                (void)reg->size();
                reentries.fetch_add(1, std::memory_order_relaxed);
                return inner->verify(id, m, s);
            }
            bool is_current_root(const AnchorKeyId& id) const override {
                (void)reg->size();
                reentries.fetch_add(1, std::memory_order_relaxed);
                return inner->is_current_root(id);
            }
        };
        auto verifier = std::make_shared<ReentrantVerifier>();
        verifier->inner->secrets[2] = kNewRootSecret;
        RevocationRegistry reg;
        verifier->reg = &reg;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy())), "enable (reentrant)", ok);
        const SignedRevocation held = make_rec(cluster, 90);
        check(static_cast<bool>(reg.revoke(held, kNow)), "revoke with a reentrant verifier", ok);
        SignedRevocation other = held;  // another root's copy: exercises the rotation root lookup
        other.anchor_key_id = anchor_n(2);
        other.signature = toy_sign(kNewRootSecret, revocation_signed_bytes(other));
        check(static_cast<bool>(reg.revoke(other, kNow)), "rotation lookup on the application path", ok);
        (void)reg.merge_signed(blobs_of({other, make_rec(cluster, 91)}), kNow);
        check(reg.is_revoked(fp_n(91)) && verifier->reentries.load() >= 4,
              "root lookups and verifies on both paths run outside the registry lock", ok);
    }

    // ============================================================================================
    // M1. Trust-root rotation: the re-signed record REPLACES the old-root copy (never the reverse), and
    //     a node that joins after the old root is gone still learns the revocation.
    // ============================================================================================
    {
        auto verifier = make_verifier();  // root 1 current
        RevocationRegistry node_a, node_b;
        check(static_cast<bool>(node_a.enable_signed(verifier, cluster, roomy())) &&
                  static_cast<bool>(node_b.enable_signed(verifier, cluster, roomy())),
              "enable (rotation)", ok);
        const SignedRevocation old_copy = make_rec(cluster, 70);
        check(static_cast<bool>(node_a.revoke(old_copy, kNow)), "revoked under root 1", ok);
        check(node_b.merge_signed(node_a.sample_signed(8), kNow), "and gossiped to node B", ok);

        // While BOTH roots are current, copies under either root are equally good: a second copy must not
        // replace the held one (otherwise two current copies would churn back and forth through gossip).
        verifier->secrets[2] = kNewRootSecret;
        verifier->current = {1, 2};
        SignedRevocation other_current = old_copy;
        other_current.anchor_key_id = anchor_n(2);
        other_current.signature = toy_sign(kNewRootSecret, revocation_signed_bytes(other_current));
        const int calls_both = verifier->calls.load();
        check(static_cast<bool>(node_a.revoke(other_current, kNow)) &&
                  node_a.signed_revocations()[0].anchor_key_id == anchor_n(1) && verifier->calls.load() == calls_both,
              "two current roots: a held copy is not replaced (no churn, no work)", ok);

        // Rotate: root 2 becomes the only current root, root 1 stays trusted as the overlap-window previous.
        verifier->current = {2};
        // Runbook: export, custodian re-signs under root 2, re-apply with revoke().
        std::vector<SignedRevocation> resigned;
        for (SignedRevocation r : node_a.signed_revocations()) {
            r.anchor_key_id = anchor_n(2);
            r.signature = toy_sign(kNewRootSecret, revocation_signed_bytes(r));
            resigned.push_back(r);
        }
        check(resigned.size() == 1 && static_cast<bool>(node_a.revoke(resigned[0], kNow)), "the re-signed record is applied", ok);
        check(node_a.signed_revocations()[0].anchor_key_id == anchor_n(2),
              "M1: the re-signed copy REPLACES the old-root copy on node A", ok);
        check(!node_b.merge_signed(node_a.sample_signed(8), kNow) && node_b.signed_revocations()[0].anchor_key_id == anchor_n(2),
              "M1: gossip carries the re-signed copy to node B, which replaces its old copy", ok);
        const int calls_before = verifier->calls.load();
        (void)node_b.merge_signed(blobs_of({old_copy}), kNow);
        check(node_b.signed_revocations()[0].anchor_key_id == anchor_n(2) && verifier->calls.load() == calls_before,
              "an old-root copy never replaces the current one back (no flip-flop, no work)", ok);

        // The overlap window closes: root 1 is no longer trusted at all.
        verifier->secrets.erase(1);
        RevocationRegistry node_c;
        check(static_cast<bool>(node_c.enable_signed(verifier, cluster, roomy())), "enable (late joiner)", ok);
        (void)node_c.merge_signed(blobs_of({old_copy}), kNow);
        check(!node_c.is_revoked(fp_n(70)), "control: the old-root copy alone no longer verifies", ok);
        (void)node_c.merge_signed(node_b.sample_signed(8), kNow);
        check(node_c.is_revoked(fp_n(70)), "M1: a node joining after the old root is gone still learns the revocation", ok);
    }

    // ============================================================================================
    // D. Strict startup check (takes the registry the node actually wires, not a flag).
    // ============================================================================================
    {
        RevocationRegistry legacy, signed_reg;
        check(static_cast<bool>(signed_reg.enable_signed(make_verifier(), cluster, roomy())), "enable (startup)", ok);
        check(!validate_revocation_trust(SecurityMode::Strict, true, legacy, kSwimWireVersion),
              "Strict + handshake + a legacy (unsigned) registry is refused at startup", ok);
        check(static_cast<bool>(validate_revocation_trust(SecurityMode::Strict, true, signed_reg, kSwimWireVersion)),
              "control: Strict + signed is fine", ok);
        check(static_cast<bool>(validate_revocation_trust(SecurityMode::Off, true, legacy, kSwimWireVersion)), "control: Off is a no-op", ok);
        check(static_cast<bool>(validate_revocation_trust(SecurityMode::Strict, false, legacy, kSwimWireVersion)),
              "control: no handshake, nothing to require", ok);
        check(!validate_revocation_trust(SecurityMode::Strict, true, signed_reg, 1),
              "Strict + signed but SWIM pinned to wire v1 (no signed section) is refused", ok);
    }

    // ============================================================================================
    // E. SWIM wire: the signed section is appended after the version trailer, and bounded.
    // ============================================================================================
    {
        ControlMsg m;
        m.kind = ControlKind::Gossip;
        m.cluster = cluster;
        m.from = NodeId{1};
        m.updates = {MemberUpdate{2, 3, 0}};
        m.revocations = {fp_n(8)};
        m.wire_version = kSwimWireVersion;
        const auto base = detail::encode_control(m);
        m.signed_revocations = blobs_of({make_rec(cluster, 1)});
        const auto with = detail::encode_control(m);
        check(with.size() > base.size() && std::equal(base.begin(), base.end(), with.begin()),
              "the signed section is a pure suffix after the version trailer (older decoders stop before it)", ok);
        ControlMsg out;
        check(detail::decode_control(with.data(), with.size(), out), "a frame with the section decodes", ok);
        check(out.signed_revocations.size() == 1 && out.signed_revocations[0] == m.signed_revocations[0] &&
                  out.updates.size() == 1 && out.revocations.size() == 1 && out.wire_version == kSwimWireVersion,
              "the section and every earlier field survive", ok);
        check(detail::decode_control(base.data(), base.size(), out) && out.signed_revocations.empty(),
              "a frame without the section decodes with none", ok);

        ControlMsg v1 = m;
        v1.wire_version = 1;
        const auto legacy = detail::encode_control(v1);
        check(detail::decode_control(legacy.data(), legacy.size(), out) && out.signed_revocations.empty(),
              "a v1 (no-trailer) frame never carries the section", ok);

        auto truncated = with;
        truncated.pop_back();
        check(!detail::decode_control(truncated.data(), truncated.size(), out), "a truncated section fails the frame", ok);
        auto at_bound = base;    // 64 well-formed blobs, the last one 256 bytes: accepted
        auto over_bound = base;  // 65 well-formed 1-byte blobs: refused by the count bound alone
        at_bound.push_back(std::byte{64});
        at_bound.push_back(std::byte{0});
        over_bound.push_back(std::byte{65});
        over_bound.push_back(std::byte{0});
        for (int i = 0; i < 65; ++i) {
            for (std::byte b : {std::byte{1}, std::byte{0}, std::byte{0xEE}}) {
                if (i < 63) at_bound.push_back(b);
                over_bound.push_back(b);
            }
        }
        at_bound.push_back(std::byte{0});  // blob 64: length 256 (0x0100)
        at_bound.push_back(std::byte{1});
        at_bound.insert(at_bound.end(), 256, std::byte{0xEE});
        check(detail::decode_control(at_bound.data(), at_bound.size(), out) && out.signed_revocations.size() == 64 &&
                  out.signed_revocations.back().size() == 256,
              "control: 64 blobs with a 256-byte one (both bounds, inclusive) decode", ok);
        check(!detail::decode_control(over_bound.data(), over_bound.size(), out), "65 blobs exceed the bound and fail the frame", ok);
        auto too_long = base;
        for (std::byte b : {std::byte{1}, std::byte{0}, std::byte{1}, std::byte{1}}) too_long.push_back(b);  // len 257
        too_long.insert(too_long.end(), 257, std::byte{0xEE});
        check(!detail::decode_control(too_long.data(), too_long.size(), out), "a 257-byte blob fails the frame", ok);
        auto empty_blob = base;
        for (std::byte b : {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}}) empty_blob.push_back(b);
        check(!detail::decode_control(empty_blob.data(), empty_blob.size(), out), "a zero-length blob fails the frame", ok);
    }

    // ============================================================================================
    // F. Gossip convergence: an anchor-signed revocation applied on node 1 reaches node 2 via SWIM.
    //    A member's forged record naming a THIRD party does not.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        LoopbackFabric fabric;
        VClock clk;
        SwimMembership::Config cfg;
        cfg.cluster_id = cluster;
        cfg.ack_timeout_ns = 100'000'000;
        cfg.suspicion_timeout_ns = 500'000'000;
        cfg.gossip_fanout = 3;
        cfg.seed = 0x5167;
        LoopbackTransport t1(fabric, NodeId{1}), t2(fabric, NodeId{2});
        SwimMembership s1(NodeId{1}, t1, cfg), s2(NodeId{2}, t2, cfg);
        s1.set_clock(&vclock_read, &clk);
        s2.set_clock(&vclock_read, &clk);
        (void)s1.admit(NodeId{2}, cluster);
        (void)s2.admit(NodeId{1}, cluster);
        RevocationRegistry r1, r2;
        check(static_cast<bool>(r1.enable_signed(verifier, cluster, roomy())) &&
                  static_cast<bool>(r2.enable_signed(verifier, cluster, roomy())),
              "enable (gossip)", ok);
        for (auto [s, r] : {std::pair{&s1, &r1}, std::pair{&s2, &r2}}) {
            RevocationRegistry* reg = r;
            s->set_signed_revocation_gossip(
                [reg](std::size_t max) { return reg->sample_signed(max); },
                [reg](const std::vector<std::vector<std::byte>>& in) { reg->merge_signed(in, kNow); });
            s->set_revocation_gossip([reg](std::size_t max) { return reg->sample(max); },
                                     [reg](const std::vector<Fingerprint>& in) { reg->merge(in); });
        }
        check(static_cast<bool>(r1.revoke(make_rec(cluster, 11), kNow)), "node 1 applies a signed revocation", ok);
        // More records than one frame carries (Config::max_signed_revocation_gossip = 8): all must converge.
        for (std::uint8_t i = 0; i < 19; ++i) (void)r1.revoke(make_rec(cluster, static_cast<std::uint8_t>(200 + i)), kNow);
        r1.revoke_locally(fp_n(12));  // unsigned: must stay on node 1
        bool converged = false;
        for (int round = 0; round < 50 && !converged; ++round) {
            clk.now += 120'000'000;
            s1.tick();
            s2.tick();
            converged = r2.is_revoked(fp_n(11)) && r2.signed_size() == 20;
        }
        check(converged, "all 20 signed revocations converge to node 2 over SWIM (8 per frame)", ok);
        check(!r2.is_revoked(fp_n(12)), "a local unsigned revocation never leaves node 1", ok);

        // Node 1 (an authenticated member) gossips a record it signed itself, claiming the anchor's key id
        // and naming a THIRD party's fingerprint (node 3's).
        ControlMsg forged;
        forged.kind = ControlKind::Gossip;
        forged.cluster = cluster;
        forged.from = NodeId{1};
        forged.from_incarnation = s1.self_incarnation();
        forged.wire_version = kSwimWireVersion;
        forged.signed_revocations = blobs_of({make_rec(cluster, 13, kLater, 1, kRogueSecret)});
        MessageFrame f{};
        f.from = NodeId{1};
        f.to = NodeId{2};
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(forged);
        const auto rejected_before = r2.signed_rejected();
        t1.send(NodeId{2}, std::move(f));
        check(r2.signed_rejected() == rejected_before + 1, "the forged record reached node 2's registry", ok);
        check(!r2.is_revoked(fp_n(13)), "C1: a member's self-signed record cannot revoke a third party", ok);
    }

    // ============================================================================================
    // G. Concurrency (run under TSan in CI): the application path, gossip merges, sampling, pruning and
    //    readers race on one registry; verification runs unlocked in between.
    // ============================================================================================
    {
        auto verifier = make_verifier();
        RevocationRegistry reg;
        check(static_cast<bool>(reg.enable_signed(verifier, cluster, roomy(256))), "enable (concurrency)", ok);
        std::vector<std::vector<std::byte>> gossip;
        for (std::uint8_t i = 0; i < 32; ++i) gossip.push_back(encode_signed_revocation(make_rec(cluster, i, kNow + 400)));
        std::atomic<bool> stop{false};
        std::vector<std::thread> threads;
        threads.emplace_back([&] {
            for (std::uint8_t i = 32; i < 96; ++i) (void)reg.revoke(make_rec(cluster, i), kNow);
        });
        threads.emplace_back([&] {
            for (int k = 0; k < 200; ++k) (void)reg.merge_signed(gossip, kNow + static_cast<std::uint64_t>(k));
        });
        threads.emplace_back([&] {
            while (!stop.load()) {
                (void)reg.sample_signed(8);
                (void)reg.snapshot();
                (void)reg.is_revoked(fp_n(40));
            }
        });
        threads.emplace_back([&] {
            for (int k = 0; k < 200; ++k) (void)reg.prune_expired(kNow + 1000);  // prunes the gossiped set
        });
        threads[0].join();
        threads[1].join();
        threads[3].join();
        stop.store(true);
        threads[2].join();
        (void)reg.prune_expired(kNow + 1000);
        check(reg.signed_size() == 64 && reg.is_revoked(fp_n(40)) && !reg.is_revoked(fp_n(0)),
              "after the race: every application record held, every expired gossip record pruned", ok);
    }

    std::printf("security_signed_revocation_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
