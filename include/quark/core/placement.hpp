// Implements 010-Distribution §"Placement across nodes" — deterministic, coordinator-free node
// selection by Rendezvous / Highest-Random-Weight (HRW) hashing, proven minimal-reassignment by
// ADR-006 (D3 upholds invariant 4 literally: every node computes the same ActorId→owner locally,
// content-addressed).
//
//   node(ActorId) = argmax over live nodes n of  weight(n, ActorId.hash())
//
// The weight mixes the node id and the actor's content hash through `detail::splitmix64` — the SAME
// deterministic mixer 026 names for VirtualBins — so the mapping is a pure function of (ActorId,
// membership set): no ring state, no coordinator, no gossip round needed to agree. Adding or removing
// a node re-places ONLY the keys whose argmax changed (HRW's minimal-disruption property), and no key
// ever moves between two nodes that both survive the change.
//
// SCOPE (this file = 010 core): UNIFORM HRW over the full live node set, O(nodes) per placement — the
// honest small/flat-cluster form. The three documented scaling seams ride ON this without changing
// which node owns an actor:
//   * 025 (ADR-013): the weighted proportional log-WRH form (`Weighted`, capacity `weight`) and
//     capability-constrained eligible subsets (`Require<Gpu>`, `Prefer<SameZone>`). Left as a seam —
//     `place()` here is the uniform base those policies specialize.
//   * 026 (ADR-006 D3): the O(1) N-independent `VirtualBins` cache (`bin = splitmix64(hash)&(B-1)`,
//     `bin_owner[bin]` refilled once/epoch). `place()` is the exact per-key oracle that table caches.
//   * 021: the real SWIM membership feeding the view (see membership.hpp).
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "quark/core/ids.hpp"
#include "quark/core/membership.hpp"
#include "quark/detail/hash.hpp"

namespace quark {

// The rendezvous weight of node `n` for a key with content hash `key_hash`. Both inputs are mixed
// through splitmix64 so each node contributes full avalanche (a node id that differs in one bit yields
// an unrelated weight), giving uniform ownership and clean minimal-reassignment. Pure + constexpr —
// identical on every node, which is the whole point of coordinator-free placement.
[[nodiscard]] constexpr std::uint64_t rendezvous_weight(NodeId n, std::uint64_t key_hash) noexcept {
    // Fold the node into the key: splitmix64 of (mixed node id XOR key hash). The inner mix of the
    // node id avoids structure when node ids are dense/sequential (0,1,2,…).
    return detail::splitmix64(detail::splitmix64(n.value) ^ key_hash);
}

// The HRW winner for `key_hash` over the live node set `nodes`. Highest weight wins; on the
// (astronomically rare) weight tie the higher NodeId::value wins, so the choice is TOTALLY ordered
// and byte-identical on every node regardless of the order nodes appear in the view. Empty set ⇒
// nullopt (no cluster to place onto — the caller falls back to local, single-node degenerate).
[[nodiscard]] inline std::optional<NodeId> place_hash(std::uint64_t key_hash,
                                                      std::span<const NodeId> nodes) noexcept {
    bool have = false;
    NodeId best{};
    std::uint64_t best_w = 0;
    for (NodeId n : nodes) {
        const std::uint64_t w = rendezvous_weight(n, key_hash);
        if (!have || w > best_w || (w == best_w && n.value > best.value)) {
            best = n;
            best_w = w;
            have = true;
        }
    }
    if (!have) return std::nullopt;
    return best;
}

// The owning node for `id` under membership view `v` (010 §Placement). Coordinator-free and stable:
// same (ActorId, node set) ⇒ same NodeId on every node. `nullopt` iff the view is empty.
[[nodiscard]] inline std::optional<NodeId> place(ActorId id, const MembershipView& v) noexcept {
    return place_hash(id.hash(), v.nodes());
}

}  // namespace quark
