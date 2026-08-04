// Implements ADR-040 Phase 5 — `RevocationRegistry`, the gossiped, union-only revocation set behind
// S3 (handshake-time rejection of a revoked-but-unexpired cert) and S5 (enforcement against sessions
// that are ALREADY open). Mirrors `InProcessMembership`'s publish_locked idiom (membership.hpp): all
// mutation happens under a mutex, readers get an immutable `shared_ptr` snapshot so a cross-thread
// read (SecureTransport::sweep_revocations, a handshake engine's factory.create()) never races a
// concurrent merge.
//
// UNION-ONLY RATCHET: a fingerprint, once revoked, is NEVER un-revoked by a merge. This is the same
// terminal-state precedent 021's SwimMembership uses for Dead (§apply_update: "Dead is terminal
// (rejoin only)") — for the identical reason: an adversary who could "unrevoke" a leaked key by
// replaying a stale gossip digest would defeat the whole mechanism. There is deliberately no un-revoke
// API here; reinstating a node means issuing it a FRESH identity (a new fingerprint), not clearing this
// set.
#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "quark/core/tls_identity.hpp"

namespace quark {

class RevocationRegistry {
public:
    RevocationRegistry() { republish_locked(); }

    // Merge a peer's gossiped revocation set into ours (union). Returns true iff this added at least
    // one NEW fingerprint — callers (e.g. a future gossip-priority policy) can use this to decide
    // whether the change is worth re-announcing sooner.
    bool merge(const std::vector<Fingerprint>& incoming) {
        if (incoming.empty()) return false;
        std::lock_guard<std::mutex> g(mu_);
        bool changed = false;
        for (const Fingerprint& fp : incoming) {
            if (set_.insert(fp).second) changed = true;
        }
        if (changed) republish_locked();
        return changed;
    }

    // Revoke a fingerprint discovered LOCALLY (operator action / detected compromise) — the origin
    // point of a revocation that then gossips outward via SwimMembership's revocation-gossip hook
    // (cluster.hpp).
    void revoke_locally(Fingerprint fp) {
        std::lock_guard<std::mutex> g(mu_);
        if (set_.insert(fp).second) republish_locked();
    }

    [[nodiscard]] bool is_revoked(const Fingerprint& fp) const {
        std::lock_guard<std::mutex> g(mu_);
        return set_.contains(fp);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return set_.size();
    }

    // Up to `max` fingerprints from the current set, for a bounded gossip piggyback (SwimMembership's
    // revocation-pull hook) — same "full/partial table, arbitrary truncation order" shape as cluster.
    // hpp's own `fill_digest`, which the header banner there documents as an accepted small-cluster
    // optimization (026 owns priority-ordered truncation for scale).
    [[nodiscard]] std::vector<Fingerprint> sample(std::size_t max) const {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<Fingerprint> out;
        out.reserve(std::min(max, set_.size()));
        for (const Fingerprint& fp : set_) {
            if (out.size() >= max) break;
            out.push_back(fp);
        }
        return out;
    }

    // An immutable snapshot for a sweep tick or a handshake-time check — never mutated once published,
    // so a reader never races a concurrent merge/revoke_locally.
    [[nodiscard]] std::shared_ptr<const std::unordered_set<Fingerprint>> snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        return snapshot_;
    }

private:
    void republish_locked() {
        snapshot_ = std::make_shared<const std::unordered_set<Fingerprint>>(set_);
    }

    mutable std::mutex mu_;
    std::unordered_set<Fingerprint> set_;
    std::shared_ptr<const std::unordered_set<Fingerprint>> snapshot_;
};

}  // namespace quark
