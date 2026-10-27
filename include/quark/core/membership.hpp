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
//   * 025: static capabilities (labels, flags, capacity `weight`), which a placement policy
//     restricts/biases/weights HRW by. `MembershipView` carries only the node SET here; the capability
//     map is an additive field `CapabilityView` (capabilities.hpp) exposes, kept live by
//     `CapabilityRegistry`'s continuous piggyback on SwimMembership's own gossip channel — NOT the
//     SWIM join payload specifically (ADR-045; capability_registry.hpp).
//
// A `MembershipView` PINS its node vector by `shared_ptr`, so a concurrent join/leave that publishes
// a new snapshot never invalidates a view already handed out (no UAF; TSan-clean). Snapshots are
// off the hot path — placement reads the (already-published) view; the copy is one refcount bump.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "quark/core/ids.hpp"

namespace quark {

// ============================================================================================
// Membership events + per-node health metadata (issue #21, ADR-053). Additive to the seam: nothing
// here changes `view()`/`epoch()`. Events are delivered SYNCHRONOUSLY on the membership driving thread
// (no new thread): a transition is queued while protocol state mutates and the queue is drained before
// the public entry point that caused it (tick / inbound frame / leave / admit) returns, so a callback
// never observes half-mutated state and may itself call back into the membership.
// ============================================================================================
// Liveness state of one member. Dead and Left are tombstones (ADR-048); Suspect is reversible.
enum class MemberStatus : std::uint8_t { Alive = 0, Suspect = 1, Dead = 2, Left = 3 };

enum class MembershipEventKind : std::uint8_t {
    Joined = 0,          // node entered the live, first-hand-verified (placement) set
    Suspect = 1,         // a joined node missed its direct+indirect probe (reversible)
    Recovered = 2,       // a Suspect node refuted / answered and is Alive again
    Dead = 3,            // suspicion timeout lapsed with no refutation (tombstone)
    Left = 4,            // graceful departure (tombstone)
    VersionRefused = 5,  // wire-protocol version outside the supported window (ADR-053); see `refusal`
};

// Distinct, observable reason a peer was refused (never conflated with a timeout / cluster mismatch).
enum class RefusalReason : std::uint8_t {
    None = 0,
    ClusterMismatch = 1,
    WireVersionTooOld = 2,  // peer speaks a version below [self - 1]
    WireVersionTooNew = 3,  // peer speaks a version above [self + 1]
};

struct MembershipEvent {
    MembershipEventKind kind{};
    NodeId node{};                    // the member the event is about
    std::uint64_t incarnation = 0;    // its incarnation at the transition
    std::uint64_t epoch = 0;          // roster epoch in effect right after the transition
    std::uint8_t protocol_version = 0;  // node's last-seen wire version (0 = never heard directly)
    std::int64_t at_ns = 0;           // injected-clock time of the transition
    RefusalReason refusal = RefusalReason::None;  // VersionRefused only
    bool refused_by_peer = false;     // VersionRefused only: true = THEY refused us (JoinReject)
};

// Small per-node health record for a "reconnecting" window / health page.
struct NodeHealth {
    NodeId node{};
    MemberStatus state = MemberStatus::Alive;
    std::uint64_t incarnation = 0;
    std::uint8_t protocol_version = 0;  // 0 = not yet heard directly from the node
    bool verified = false;              // first-hand evidence seen (in the placement view)
    RefusalReason last_refusal = RefusalReason::None;  // sticky: why we last refused this node
    std::uint8_t refused_version = 0;                  // the version it presented then
};

using MembershipEventSink = std::function<void(const MembershipEvent&)>;

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

    // The subset placement may choose from. Default = view(); SWIM overrides it to exclude members
    // learned only from gossip until they answer directly (F12). SWIM's own convergence/probe logic
    // and `view()` still see them, so the roster converges; they simply cannot receive an actor.
    [[nodiscard]] virtual MembershipView placement_view() const noexcept { return view(); }

    // Subscribe to Joined/Suspect/Recovered/Dead/Left/VersionRefused (setup-time; no unsubscribe).
    // Default: no events. Callbacks run on the driving thread, before the causing call returns.
    virtual void add_event_sink(MembershipEventSink) {}
    // Health metadata for one node (nullopt = unknown). Default: none.
    [[nodiscard]] virtual std::optional<NodeHealth> health(NodeId) const { return std::nullopt; }
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
        MembershipEvent ev{};
        {
            std::lock_guard<std::mutex> g(mu_);
            std::vector<NodeId> next = *snapshot_;
            if (std::find(next.begin(), next.end(), n) != next.end()) return;  // already live
            next.push_back(n);
            publish_locked(std::move(next));
            ev = {MembershipEventKind::Joined, n, 0, epoch_, 0, 0};
        }
        emit(ev);
    }

    // Remove a node from the live set (idempotent). Only actors OWNED by `n` re-place; the rest keep
    // their owner. A node cannot leave itself out of its own view here (test invariant), but the seam
    // does not forbid it — SWIM handles self-departure/drain in 021.
    void leave(NodeId n) {
        MembershipEvent ev{};
        {
            std::lock_guard<std::mutex> g(mu_);
            std::vector<NodeId> next = *snapshot_;
            const auto it = std::remove(next.begin(), next.end(), n);
            if (it == next.end()) return;  // not present
            next.erase(it, next.end());
            publish_locked(std::move(next));
            ev = {MembershipEventKind::Left, n, 0, epoch_, 0, 0};
        }
        emit(ev);
    }

    // Test double: only Joined/Left exist (it is not a failure detector); delivered inline from
    // join()/leave() on the calling thread, outside the lock.
    void add_event_sink(MembershipEventSink s) override { sinks_.push_back(std::move(s)); }
    [[nodiscard]] std::optional<NodeHealth> health(NodeId n) const override {
        if (!view().contains(n)) return std::nullopt;
        return NodeHealth{n, MemberStatus::Alive, 0, 0, true, RefusalReason::None, 0};
    }

    [[nodiscard]] std::uint64_t epoch() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return epoch_;
    }

private:
    void emit(const MembershipEvent& e) {
        for (auto& s : sinks_) s(e);
    }
    std::vector<MembershipEventSink> sinks_;  // setup-time only (no lock needed to iterate)
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
