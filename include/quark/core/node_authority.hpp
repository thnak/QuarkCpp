// Implements 020-Security §1 (node identity + cluster admission) — the `NodeAuthority` trust-anchor
// SEAM plus admission-gated placement. This is the sharpest distributed control: because placement is
// any-node-hosts-any-actor (010), cluster admission IS data authorization at node granularity. An
// unauthenticated/unauthorized node must be INVISIBLE to HRW placement — `place_admitted()` filters
// the membership view to admitted nodes BEFORE running the rendezvous argmax, so an unadmitted node can
// never win, can never be assigned an actor, and therefore can never pull actor state.
//
// SEAM SPLIT (020 §1): 021 ESTABLISHES the anchor — how node identities are provisioned/bootstrapped,
// the shared cluster CA / pre-shared key / SPIFFE plane, the cluster-id/epoch merge guard. 020
// VALIDATES against it: given "is node N admitted?", it gates placement. The crypto proof-of-identity
// (the mutual-auth handshake that must PRECEDE admission) is the `SecureTransport` handshake
// (secure_transport.hpp) — a DEFERRED real-crypto adapter; this header consumes the admission verdict,
// it does not perform the handshake.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/placement.hpp"

namespace quark {

// ============================================================================================
// The NodeAuthority trust-anchor seam (020 §1 / 021). `admitted(n)` is the verdict: has node `n`
// completed the mutual-auth handshake and been admitted under the cluster's trust anchor? Placement
// consults it, so an un-admitted peer is filtered out of the eligible set. A real anchor (cluster CA /
// pre-shared key / SPIFFE / PKI / allowlist) implements this; 021 provisions it.
// ============================================================================================
class NodeAuthority {
public:
    virtual ~NodeAuthority() = default;
    [[nodiscard]] virtual bool admitted(NodeId n) const noexcept = 0;
};

// A NodeAuthority that admits EVERYTHING — the single-host / dev default (no admission control). Named
// as such so it is obvious in a config that admission is disabled.
class AdmitAllNodeAuthority final : public NodeAuthority {
public:
    [[nodiscard]] bool admitted(NodeId) const noexcept override { return true; }
};

// The default explicit trust anchor: an allowlist of admitted node ids (021 would populate it as nodes
// complete the handshake; a static allowlist is the pre-shared-roster form). Mutations are cold setup.
class AllowlistNodeAuthority final : public NodeAuthority {
public:
    AllowlistNodeAuthority() = default;
    explicit AllowlistNodeAuthority(std::span<const NodeId> admitted) {
        for (NodeId n : admitted) admitted_.insert(n.value);
    }

    void admit(NodeId n) { admitted_.insert(n.value); }
    void revoke(NodeId n) { admitted_.erase(n.value); }

    [[nodiscard]] bool admitted(NodeId n) const noexcept override {
        return admitted_.find(n.value) != admitted_.end();
    }

private:
    std::unordered_set<std::uint64_t> admitted_;
};

// Filter `view`'s live nodes to the ADMITTED subset (020 §1). Off the hot path — placement reads the
// already-published view. The result preserves the view's ascending-NodeId order so HRW iteration and
// any downstream `contains` stay stable.
inline void admitted_nodes(const MembershipView& view, const NodeAuthority& authority,
                           std::vector<NodeId>& out) {
    out.clear();
    for (NodeId n : view.nodes())
        if (authority.admitted(n)) out.push_back(n);
}

// Admission-gated placement (020 §1). Runs the HRW argmax over ONLY the admitted nodes, so an
// unadmitted node — even one that is live in the membership view — can NEVER be the placement winner.
// `nullopt` iff no admitted node exists (nothing to place onto). This is the coordinator-free
// placement of 010 with the eligible set intersected against the trust anchor.
[[nodiscard]] inline std::optional<NodeId> place_admitted(ActorId id, const MembershipView& view,
                                                          const NodeAuthority& authority) {
    std::vector<NodeId> eligible;
    admitted_nodes(view, authority, eligible);
    return place_hash(id.hash(), std::span<const NodeId>(eligible));
}

}  // namespace quark
