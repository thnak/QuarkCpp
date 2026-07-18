// Implements 010-Distribution §"Membership" — the cluster-membership SEAM plus a std-only,
// in-process test membership so placement (placement.hpp) and distributed routing (distribution.hpp)
// are testable without a network.
//
// THE SEAM (`Membership`): the live NodeId set as an immutable, epoch-stamped snapshot (`view()`),
// plus this node's own id (`self()`). Placement is a pure function of a `MembershipView`, so the seam
// only has to publish "who is live right now, and which generation of the roster that is".
//
// WHAT LIVES BEHIND IT (NOT here — explicit seams):
//   * 021 (Cluster Formation & Lifecycle): the real in-house SWIM protocol — gossip dissemination,
//     randomized ping / indirect-ping failure detection, suspicion timeout + incarnation refutation,
//     and the staged join FSM / stabilization window that damps flap thrash. A `SwimMembership`
//     adapter would implement this same `Membership` interface over the Transport. We do NOT
//     implement SWIM here; `InProcessMembership` below is a deterministic test double, not a
//     failure detector.
//   * 025: static capabilities (labels, flags, capacity `weight`) gossiped in the SWIM join payload,
//     which a placement policy restricts/biases/weights HRW by. `MembershipView` carries only the
//     node SET here; the capability map is an additive field a capability-aware view exposes.
//
// A `MembershipView` PINS its node vector by `shared_ptr`, so a concurrent join/leave that publishes
// a new snapshot never invalidates a view already handed out (no UAF; TSan-clean). Snapshots are
// off the hot path — placement reads the (already-published) view; the copy is one refcount bump.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "quark/core/ids.hpp"

namespace quark {

// An immutable snapshot of the live node set + the roster epoch it was taken at. Value type: copy is
// a refcount bump; the pinned vector keeps the span valid for the view's whole lifetime.
class MembershipView {
public:
    MembershipView() = default;
    MembershipView(std::shared_ptr<const std::vector<NodeId>> nodes, std::uint64_t epoch) noexcept
        : nodes_(std::move(nodes)), epoch_(epoch) {}

    // The live nodes, ascending by NodeId::value (so HRW iteration order and `contains` are stable).
    [[nodiscard]] std::span<const NodeId> nodes() const noexcept {
        return nodes_ ? std::span<const NodeId>(*nodes_) : std::span<const NodeId>{};
    }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_ ? nodes_->size() : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

    [[nodiscard]] bool contains(NodeId n) const noexcept {
        if (!nodes_) return false;
        return std::binary_search(nodes_->begin(), nodes_->end(), n,
                                  [](NodeId a, NodeId b) { return a.value < b.value; });
    }

private:
    std::shared_ptr<const std::vector<NodeId>> nodes_;
    std::uint64_t epoch_ = 0;
};

// ============================================================================================
// The membership seam (010). The real failure detector (021 SWIM) implements this same interface.
// ============================================================================================
class Membership {
public:
    virtual ~Membership() = default;

    // This node's stable identity.
    [[nodiscard]] virtual NodeId self() const noexcept = 0;

    // The current live-node snapshot. Cheap (a published pointer + epoch); safe to call per-send.
    [[nodiscard]] virtual MembershipView view() const noexcept = 0;
};

// ============================================================================================
// In-process test membership (std-only). Deterministic join/leave/current-view — NOT a network
// failure detector (that is 021 SWIM). Thread-safe: mutations rebuild + republish an immutable
// sorted snapshot under a mutex and bump the epoch; readers copy the published snapshot.
// ============================================================================================
class InProcessMembership final : public Membership {
public:
    explicit InProcessMembership(NodeId self) : self_(self) {
        std::vector<NodeId> init{self};
        publish(std::move(init));
    }

    // Construct with an explicit starting roster (must include `self` for a well-formed node).
    InProcessMembership(NodeId self, std::vector<NodeId> initial) : self_(self) {
        publish(std::move(initial));
    }

    [[nodiscard]] NodeId self() const noexcept override { return self_; }

    [[nodiscard]] MembershipView view() const noexcept override {
        std::lock_guard<std::mutex> g(mu_);
        return MembershipView{snapshot_, epoch_};
    }

    // Add a node to the live set (idempotent). Re-places the fraction of actors whose HRW argmax
    // becomes `n` — every OTHER actor keeps its owner (HRW minimal disruption).
    void join(NodeId n) {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<NodeId> next = *snapshot_;
        if (std::find(next.begin(), next.end(), n) != next.end()) return;  // already live
        next.push_back(n);
        publish_locked(std::move(next));
    }

    // Remove a node from the live set (idempotent). Only actors OWNED by `n` re-place; the rest keep
    // their owner. A node cannot leave itself out of its own view here (test invariant), but the seam
    // does not forbid it — SWIM handles self-departure/drain in 021.
    void leave(NodeId n) {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<NodeId> next = *snapshot_;
        const auto it = std::remove(next.begin(), next.end(), n);
        if (it == next.end()) return;  // not present
        next.erase(it, next.end());
        publish_locked(std::move(next));
    }

    [[nodiscard]] std::uint64_t epoch() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return epoch_;
    }

private:
    void publish(std::vector<NodeId> nodes) {
        std::lock_guard<std::mutex> g(mu_);
        publish_locked(std::move(nodes));
    }
    void publish_locked(std::vector<NodeId> nodes) {
        std::sort(nodes.begin(), nodes.end(), [](NodeId a, NodeId b) { return a.value < b.value; });
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        snapshot_ = std::make_shared<const std::vector<NodeId>>(std::move(nodes));
        ++epoch_;
    }

    NodeId self_;
    mutable std::mutex mu_;
    std::shared_ptr<const std::vector<NodeId>> snapshot_;
    std::uint64_t epoch_ = 0;
};

}  // namespace quark
