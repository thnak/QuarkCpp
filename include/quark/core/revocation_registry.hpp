// Implements ADR-040 Phase 5 — `RevocationRegistry`, the gossiped, union-only revocation set behind
// S3 (handshake-time rejection of a revoked-but-unexpired cert) and S5 (enforcement against sessions
// that are ALREADY open). Mirrors `InProcessMembership`'s publish_locked idiom (membership.hpp): all
// mutation happens under a mutex, readers get an immutable `shared_ptr` snapshot so a cross-thread
// read (SecureTransport::sweep_revocations, a handshake engine's factory.create()) never races a
// concurrent merge. ADR-067 D2 adds the ISSUER-AUTHENTICATED (signed) form; see TRUST AND BOUNDS.
//
// UNION-ONLY RATCHET: a fingerprint, once revoked, is NEVER un-revoked by a merge. This is the same
// terminal-state precedent 021's SwimMembership uses for Dead (§apply_update: "Dead is terminal
// (rejoin only)") — for the identical reason: an adversary who could "unrevoke" a leaked key by
// replaying a stale gossip digest would defeat the whole mechanism. There is deliberately no un-revoke
// API here; reinstating a node means issuing it a FRESH identity (a new fingerprint), not clearing this
// set. Two removals exist, both ADR-067 D2: `prune_expired` drops a SIGNED entry whose revoked leaf has
// expired (`not_after` + skew; an expired leaf already fails the handshake, so pruning cannot re-admit
// it), and `enable_signed` drops UNSIGNED entries that were learned from gossip (a member's unverifiable
// claim — exactly what signed mode exists to refuse); operator-local revocations are kept.
//
// TRUST AND BOUNDS. Two modes:
//   * UNSIGNED (the ADR-040 legacy mode, no verifier): revocations ride the mTLS-authenticated-member
//     gossip trust boundary, so any admitted member can publish one — including for a THIRD party.
//     Bounded: the gossip-fed set is capped (`max_entries`); NEW gossiped fingerprints past it are refused
//     and counted (`rejected()`). `protect(fp)` pins THIS node's own fingerprint so a peer cannot sever us.
//     Locally-originated revocations (operator action) are never refused. `SecurityMode::Strict` refuses
//     this mode when the mTLS handshake is on (security.hpp validate_revocation_trust).
//   * SIGNED (ADR-067 D2, after `enable_signed`): a gossiped revocation must be a `SignedRevocation`
//     signed by the cluster trust anchor and verified through `TrustAnchorVerifier`, so no member can
//     revoke a third party. Unsigned gossip is refused (counted in `rejected()`) and local unsigned
//     revocations stay node-local (`sample()` returns nothing). A signed record is authoritative:
//     `protect` does not apply. Bounds (SignedRevocationLimits):
//       - signed entries are capped; a full cap refuses LOUDLY on the application path (`revoke()` →
//         errc::overloaded) and is counted on the gossip path, checked BEFORE any signature work;
//       - records already past `not_after + expiry_skew_s` are refused before verification, so pruned
//         records cannot be replayed back in to refill the cap;
//       - gossip verification is budgeted: at most `max_verifies_per_merge` checks per merge call and a
//         token bucket (`verify_burst`, `verifies_per_second`) across merges, so this node never spends
//         unbounded CPU. Over-budget records are dropped unverified (`signed_deferred()`) and arrive again
//         later: `sample_signed` walks the whole held set round-robin. The bucket is node-wide, not per
//         sender, so a member flooding forged records can keep genuine NEW ones deferred for as long as the
//         flood lasts (honest nodes only re-gossip records they verified, so the flooder is the immediate
//         sender of the bad signatures). The application path (`revoke`) is not budgeted;
//       - a held fingerprint is not re-verified, except when the incoming record is signed by a CURRENT
//         root and the held one is not (a trust-root ROTATION: the re-signed copy replaces the old-root
//         copy, never the reverse).
//     Root lookups and signature checks run OUTSIDE the registry lock (the verifier has its own).
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "quark/core/error.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/signed_revocation.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark {

// ADR-067 D2: the bounds of signed mode (see TRUST AND BOUNDS above).
struct SignedRevocationLimits {
    std::size_t max_signed = 4096;              // signed entries held
    std::size_t max_verifies_per_merge = 8;     // signature checks one gossip merge may spend
    std::uint32_t verify_burst = 32;            // gossip verify token bucket: capacity
    std::uint32_t verifies_per_second = 32;     //                            refill rate
    std::uint64_t expiry_skew_s = 300;          // tolerance on not_after (clock skew between nodes)
};

class RevocationRegistry {
public:
    explicit RevocationRegistry(std::size_t max_entries = 4096) : max_entries_(max_entries) {
        republish_locked();
    }

    // Pin a fingerprint (this node's own) so a GOSSIPED revocation of it is ignored. Local
    // `revoke_locally` still wins (an operator may deliberately revoke their own identity).
    void protect(Fingerprint fp) {
        std::lock_guard<std::mutex> g(mu_);
        protected_.insert(fp);
    }

    // Gossiped fingerprints refused (set full, a protected fingerprint, or — in signed mode — any
    // unsigned gossip). Observability only.
    [[nodiscard]] std::uint64_t rejected() const {
        std::lock_guard<std::mutex> g(mu_);
        return rejected_;
    }

    // Merge a peer's gossiped revocation set into ours (union). Returns true iff this added at least
    // one NEW fingerprint — callers (e.g. a future gossip-priority policy) can use this to decide
    // whether the change is worth re-announcing sooner. In signed mode every entry is refused.
    bool merge(const std::vector<Fingerprint>& incoming) {
        if (incoming.empty()) return false;
        std::lock_guard<std::mutex> g(mu_);
        if (verifier_) {
            rejected_ += incoming.size();
            return false;
        }
        bool changed = false;
        for (const Fingerprint& fp : incoming) {
            if (is_revoked_locked(fp)) continue;  // already revoked is not a change
            if (protected_.contains(fp) || gossiped_.size() >= max_entries_) {
                ++rejected_;
                continue;
            }
            gossiped_.insert(fp);
            changed = true;
        }
        if (changed) republish_locked();
        return changed;
    }

    // Revoke a fingerprint discovered LOCALLY (operator action / detected compromise) — the origin
    // point of a revocation that then gossips outward via SwimMembership's revocation-gossip hook
    // (cluster.hpp). In signed mode it stays node-local; use `revoke(SignedRevocation)` to propagate.
    void revoke_locally(Fingerprint fp) {
        std::lock_guard<std::mutex> g(mu_);
        if (local_.insert(fp).second) republish_locked();
    }

    [[nodiscard]] bool is_revoked(const Fingerprint& fp) const {
        std::lock_guard<std::mutex> g(mu_);
        return is_revoked_locked(fp);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return snapshot_->size();
    }

    // Up to `max` unsigned fingerprints, for a bounded gossip piggyback (SwimMembership's revocation-pull
    // hook) — same "full/partial table, arbitrary truncation order" shape as cluster.hpp's own
    // `fill_digest`, which the header banner there documents as an accepted small-cluster optimization
    // (026 owns priority-ordered truncation for scale). Empty in signed mode.
    [[nodiscard]] std::vector<Fingerprint> sample(std::size_t max) const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<Fingerprint> out;
        if (verifier_) return out;
        for (const auto* set : {&local_, &gossiped_})
            for (const Fingerprint& fp : *set) {
                if (out.size() >= max) return out;
                out.push_back(fp);
            }
        return out;
    }

    // An immutable snapshot (local ∪ gossiped ∪ signed) for a sweep tick or a handshake-time check —
    // never mutated once published, so a reader never races a concurrent merge/revoke_locally.
    [[nodiscard]] std::shared_ptr<const std::unordered_set<Fingerprint>> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return snapshot_;
    }

    // ---- ADR-067 D2: issuer-authenticated revocation -------------------------------------------

    // Switch to SIGNED mode, once: from now on gossip must carry anchor-signed records for `cluster`,
    // verified by `verifier`. Unsigned entries previously learned FROM GOSSIP are dropped (a member's
    // unverifiable claim); operator-local ones are kept. errc::validation for a null verifier, a zero
    // limit (it would silently disable gossip verification), or a second call (the mode is one-way and the
    // verifier is never swapped under an in-flight check).
    [[nodiscard]] result<void> enable_signed(std::shared_ptr<const TrustAnchorVerifier> verifier, ClusterId cluster,
                                             SignedRevocationLimits limits = {}) {
        if (!verifier) return fail(errc::validation, "enable_signed needs a TrustAnchorVerifier");
        if (limits.max_signed == 0 || limits.max_verifies_per_merge == 0 || limits.verify_burst == 0 ||
            limits.verifies_per_second == 0)
            return fail(errc::validation, "SignedRevocationLimits must all be non-zero");
        std::lock_guard<std::mutex> g(mu_);
        if (verifier_) return fail(errc::validation, "signed revocation mode is already enabled");
        verifier_ = std::move(verifier);
        cluster_ = cluster;
        limits_ = limits;
        tokens_ = limits_.verify_burst;
        gossiped_.clear();
        republish_locked();
        return {};
    }
    [[nodiscard]] bool signed_mode() const {
        std::lock_guard<std::mutex> g(mu_);
        return verifier_ != nullptr;
    }

    // Apply an anchor-signed record supplied by the application (its key custodian signed it); `now_unix_s`
    // is the caller's wall clock. Not budgeted.
    //   errc::unavailable — no verifier installed (refused rather than silently revoking on one node);
    //   errc::validation  — wrong cluster, unknown root, a bad signature, or an already-expired record;
    //   errc::overloaded  — the signed cap is full (loud; raise it or prune).
    // A record for a held fingerprint succeeds; it REPLACES the held copy when it is signed by a current
    // root and the held copy is not (the rotation re-sign step).
    [[nodiscard]] result<void> revoke(const SignedRevocation& rec, std::uint64_t now_unix_s) {
        std::shared_ptr<const TrustAnchorVerifier> v;
        ClusterId cluster{};
        Candidate c{rec, false, {}};
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!verifier_) return fail(errc::unavailable, "signed revocation needs a TrustAnchorVerifier");
            if (expired_locked(rec, now_unix_s)) return fail(errc::validation, "revocation record already expired");
            const auto it = signed_.find(rec.fingerprint);
            if (it != signed_.end()) {
                if (it->second.anchor_key_id == rec.anchor_key_id) return {};
                c.held = true;
                c.held_anchor = it->second.anchor_key_id;
            } else if (signed_.size() >= limits_.max_signed) {
                return fail(errc::overloaded, "signed revocation cap is full");
            }
            v = verifier_;
            cluster = cluster_;
        }
        if (c.held && !rotates(*v, c)) return {};  // already revoked; not a rotation replacement
        if (!verify_signed_revocation(rec, cluster, *v)) {
            std::lock_guard<std::mutex> g(mu_);
            ++signed_rejected_;
            return fail(errc::validation, "revocation record does not verify under a trusted root");
        }
        std::lock_guard<std::mutex> g(mu_);
        switch (apply_locked(c, now_unix_s)) {
            case Apply::Changed: republish_locked(); return {};
            case Apply::Unchanged: return {};
            case Apply::CapFull: break;
        }
        return fail(errc::overloaded, "signed revocation cap is full");
    }

    // Absorb gossiped signed records (opaque wire blobs, encode_signed_revocation form); `now_unix_s` is
    // the caller's wall clock. Malformed, wrong-cluster and unverifiable records count in
    // `signed_rejected()`, expired ones in `signed_expired()`, a full cap in `signed_cap_refused()`, and
    // records beyond the verify budget in `signed_deferred()`. Returns true iff the enforced set changed.
    // Without a verifier everything is ignored.
    bool merge_signed(std::span<const std::vector<std::byte>> blobs, std::uint64_t now_unix_s) {
        if (blobs.empty()) return false;
        std::shared_ptr<const TrustAnchorVerifier> v;
        ClusterId cluster{};
        std::vector<Candidate> candidates;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!verifier_) return false;
            v = verifier_;
            cluster = cluster_;
            refill_locked(now_unix_s);
            std::unordered_set<Fingerprint> seen;
            std::size_t new_entries = 0;
            for (const auto& blob : blobs) {
                Candidate c{};
                if (!decode_signed_revocation(blob, c.rec) || c.rec.cluster != cluster_) {
                    ++signed_rejected_;
                    continue;
                }
                if (!seen.insert(c.rec.fingerprint).second) continue;  // one candidate per fingerprint
                if (expired_locked(c.rec, now_unix_s)) {
                    ++signed_expired_;
                    continue;
                }
                const auto it = signed_.find(c.rec.fingerprint);
                if (it != signed_.end()) {
                    if (it->second.anchor_key_id == c.rec.anchor_key_id) continue;  // the copy we hold: no work
                    c.held = true;  // another root's copy: a possible rotation, decided outside the lock
                    c.held_anchor = it->second.anchor_key_id;
                } else if (signed_.size() + new_entries >= limits_.max_signed) {
                    ++signed_cap_refused_;  // checked BEFORE spending a signature check
                    continue;
                }
                if (candidates.size() >= limits_.max_verifies_per_merge || tokens_ == 0) {
                    ++signed_deferred_;
                    continue;
                }
                --tokens_;
                if (!c.held) ++new_entries;
                candidates.push_back(std::move(c));
            }
        }
        std::vector<Candidate> verified;
        std::uint64_t bad = 0;
        std::uint32_t refund = 0;
        for (Candidate& c : candidates) {  // root lookups and crypto OUTSIDE the lock
            if (c.held && !rotates(*v, c)) {
                ++refund;  // not a rotation replacement: no signature check was spent
                continue;
            }
            if (verify_signed_revocation(c.rec, cluster, *v)) verified.push_back(std::move(c));
            else ++bad;
        }
        std::lock_guard<std::mutex> g(mu_);
        signed_rejected_ += bad;
        tokens_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(limits_.verify_burst, std::uint64_t{tokens_} + refund));
        bool changed = false;
        for (const Candidate& c : verified) {
            switch (apply_locked(c, now_unix_s)) {
                case Apply::Changed: changed = true; break;
                case Apply::Unchanged: break;
                case Apply::CapFull: ++signed_cap_refused_; break;
            }
        }
        if (changed) republish_locked();
        return changed;
    }

    // Up to `max` signed records in wire form, for the SWIM signed-revocation piggyback. Successive calls
    // walk the whole set round-robin (insertion order), so every held record is gossiped even when the set
    // is larger than one frame carries.
    [[nodiscard]] std::vector<std::vector<std::byte>> sample_signed(std::size_t max) {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::vector<std::byte>> out;
        const std::size_t n = std::min(max, order_.size());
        out.reserve(n);
        if (n == 0) return out;
        const std::size_t start = sample_cursor_ % order_.size();
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(encode_signed_revocation(signed_.at(order_[(start + i) % order_.size()])));
        sample_cursor_ = (start + n) % order_.size();
        return out;
    }

    // Every held signed record — exported so a trust-root ROTATION can have them re-signed under the new
    // root (then re-applied with `revoke`) before the old root's overlap window closes.
    [[nodiscard]] std::vector<SignedRevocation> signed_revocations() const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<SignedRevocation> out;
        out.reserve(signed_.size());
        for (const auto& [fp, rec] : signed_) out.push_back(rec);
        return out;
    }

    // Drop signed entries whose revoked leaf expired more than `expiry_skew_s` before `now_unix_s`.
    // A fingerprint also revoked locally stays revoked. Returns the number pruned.
    std::size_t prune_expired(std::uint64_t now_unix_s) {
        std::lock_guard<std::mutex> g(mu_);
        last_prune_s_ = std::max(last_prune_s_, now_unix_s);
        const std::size_t pruned =
            std::erase_if(signed_, [&](const auto& kv) { return expired_locked(kv.second, now_unix_s); });
        if (pruned != 0) {
            std::erase_if(order_, [&](const Fingerprint& fp) { return !signed_.contains(fp); });
            republish_locked();
        }
        return pruned;
    }

    [[nodiscard]] std::size_t signed_size() const {
        std::lock_guard<std::mutex> g(mu_);
        return signed_.size();
    }
    [[nodiscard]] std::uint64_t signed_rejected() const {
        std::lock_guard<std::mutex> g(mu_);
        return signed_rejected_;
    }
    [[nodiscard]] std::uint64_t signed_cap_refused() const {
        std::lock_guard<std::mutex> g(mu_);
        return signed_cap_refused_;
    }
    [[nodiscard]] std::uint64_t signed_expired() const {
        std::lock_guard<std::mutex> g(mu_);
        return signed_expired_;
    }
    [[nodiscard]] std::uint64_t signed_deferred() const {
        std::lock_guard<std::mutex> g(mu_);
        return signed_deferred_;
    }

private:
    enum class Apply : std::uint8_t { Changed, Unchanged, CapFull };

    // A record selected for verification. `held`: we already hold another root's copy of this fingerprint
    // (`held_anchor` signed it), so the record can only REPLACE that copy (rotation), never add an entry.
    struct Candidate {
        SignedRevocation rec;
        bool held = false;
        AnchorKeyId held_anchor{};
    };

    [[nodiscard]] bool is_revoked_locked(const Fingerprint& fp) const {
        return local_.contains(fp) || gossiped_.contains(fp) || signed_.contains(fp);
    }

    [[nodiscard]] bool expired_locked(const SignedRevocation& rec, std::uint64_t now_unix_s) const {
        return rec.not_after <= now_unix_s && now_unix_s - rec.not_after > limits_.expiry_skew_s;
    }

    // Rotation: a held copy is replaced only by a copy signed by a CURRENT root when the held one's root
    // is not current — never the reverse, so old and new copies cannot flip-flop. Called WITHOUT the
    // registry lock: the verifier takes its own lock, which must not nest inside ours (a slow verify
    // elsewhere would stall every is_revoked/snapshot reader).
    [[nodiscard]] static bool rotates(const TrustAnchorVerifier& v, const Candidate& c) {
        return v.is_current_root(c.rec.anchor_key_id) && !v.is_current_root(c.held_anchor);
    }

    // Insert a VERIFIED record, or replace the held copy the rotation decision was made against. The state
    // may have moved while the lock was released, so re-check: a copy that changed meanwhile is left alone,
    // and a record that expired by the latest prune is not brought back.
    Apply apply_locked(const Candidate& c, std::uint64_t now_unix_s) {
        if (expired_locked(c.rec, std::max(now_unix_s, last_prune_s_))) return Apply::Unchanged;
        const auto it = signed_.find(c.rec.fingerprint);
        if (it != signed_.end()) {
            if (c.held && it->second.anchor_key_id == c.held_anchor) it->second = c.rec;
            return Apply::Unchanged;  // same fingerprint: the enforced set does not change
        }
        if (signed_.size() >= limits_.max_signed) return Apply::CapFull;
        signed_.emplace(c.rec.fingerprint, c.rec);
        order_.push_back(c.rec.fingerprint);
        return Apply::Changed;
    }

    // Gossip verify token bucket, refilled from the caller's clock in whole seconds. A clock step
    // backwards rebases the bucket (refills nothing) rather than stalling it until the old reading is
    // reached again; a long gap simply fills it (verifies_per_second >= 1, so no overflow).
    void refill_locked(std::uint64_t now_unix_s) {
        if (now_unix_s < last_refill_s_) {
            last_refill_s_ = now_unix_s;
            return;
        }
        const std::uint64_t elapsed = now_unix_s - last_refill_s_;
        last_refill_s_ = now_unix_s;
        if (elapsed >= limits_.verify_burst) {
            tokens_ = limits_.verify_burst;
            return;
        }
        tokens_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(limits_.verify_burst, tokens_ + elapsed * limits_.verifies_per_second));
    }

    void republish_locked() {
        auto all = std::make_shared<std::unordered_set<Fingerprint>>(local_);
        all->insert(gossiped_.begin(), gossiped_.end());
        for (const auto& [fp, rec] : signed_) all->insert(fp);
        snapshot_ = std::move(all);
    }

    mutable std::mutex mu_;
    std::unordered_set<Fingerprint> local_;     // operator revocations (revoke_locally)
    std::unordered_set<Fingerprint> gossiped_;  // unsigned, learned from gossip (legacy mode only)
    std::unordered_map<Fingerprint, SignedRevocation> signed_;
    std::vector<Fingerprint> order_;            // signed_ keys in insertion order (sample_signed rotation)
    std::size_t sample_cursor_ = 0;
    std::unordered_set<Fingerprint> protected_;
    std::size_t max_entries_;
    std::uint64_t rejected_ = 0;
    std::shared_ptr<const TrustAnchorVerifier> verifier_;  // set once (enable_signed), never swapped
    ClusterId cluster_{};
    SignedRevocationLimits limits_{};
    std::uint32_t tokens_ = 0;
    std::uint64_t last_refill_s_ = 0;
    std::uint64_t last_prune_s_ = 0;
    std::uint64_t signed_rejected_ = 0;
    std::uint64_t signed_cap_refused_ = 0;
    std::uint64_t signed_expired_ = 0;
    std::uint64_t signed_deferred_ = 0;
    std::shared_ptr<const std::unordered_set<Fingerprint>> snapshot_;
};

}  // namespace quark
