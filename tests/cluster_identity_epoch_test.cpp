// Tests 021-Cluster-Formation-and-Lifecycle §1 (cluster identity + membership epoch):
//   * CLUSTER-ID MISMATCH REJECTED — a joiner presenting the wrong cluster id is refused, both via the
//     direct admit() gate and via the Join/JoinReject control handshake (the accidental-merge guard
//     that 010's coordinator-free HRW cannot otherwise catch).
//   * EPOCH MONOTONICITY — the epoch strictly increases on each ADMITTED roster change (join / dead /
//     leave), giving a total order on snapshots; a pinned stale snapshot is distinguishable from the
//     current one by its lower epoch while still being valid to read (no UAF).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

constexpr ClusterId kCluster{0xAAAA};
constexpr ClusterId kOther{0xBBBB};

SwimMembership::Config cfg_for(ClusterId cid) {
    SwimMembership::Config c;
    c.cluster_id = cid;
    return c;
}

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Cluster-id mismatch rejected — direct admit() gate. --------------------------------------
    {
        LoopbackFabric fabric;
        LoopbackTransport t(fabric, NodeId{1});
        SwimMembership swim(NodeId{1}, t, cfg_for(kCluster));

        check(!swim.admit(NodeId{9}, kOther), "admit() refuses a wrong-cluster joiner", ok);
        check(!swim.view().contains(NodeId{9}), "a rejected joiner never enters the roster", ok);
        check(swim.admit(NodeId{9}, kCluster), "admit() accepts a matching-cluster joiner", ok);
        check(swim.view().contains(NodeId{9}), "a matching joiner enters the roster", ok);
    }

    // ---- Cluster-id mismatch rejected — Join/JoinReject control handshake. ------------------------
    {
        LoopbackFabric fabric;
        LoopbackTransport ts(fabric, NodeId{1});
        SwimMembership seed(NodeId{1}, ts, cfg_for(kCluster));

        // A joiner in the WRONG cluster presents its (mismatched) id in the Join handshake → refused.
        LoopbackTransport tw(fabric, NodeId{2});
        SwimMembership wrong(NodeId{2}, tw, cfg_for(kOther));
        wrong.request_join(NodeId{1});  // inline over the loopback → seed replies JoinReject
        check(!seed.view().contains(NodeId{2}), "seed refuses a wrong-cluster Join (accidental-merge)",
              ok);

        // A joiner in the RIGHT cluster is admitted via JoinAck.
        LoopbackTransport tr(fabric, NodeId{3});
        SwimMembership right(NodeId{3}, tr, cfg_for(kCluster));
        right.request_join(NodeId{1});
        check(seed.view().contains(NodeId{3}), "seed admits a matching-cluster Join", ok);
    }

    // ---- Epoch monotonicity + stale-snapshot distinguishability. ---------------------------------
    {
        LoopbackFabric fabric;
        LoopbackTransport t(fabric, NodeId{1});
        SwimMembership swim(NodeId{1}, t, cfg_for(kCluster));

        const MembershipView v_start = swim.view();  // pinned snapshot BEFORE any change
        const std::uint64_t e0 = swim.epoch();

        std::uint64_t prev = e0;
        (void)swim.admit(NodeId{2}, kCluster);
        check(swim.epoch() > prev, "epoch strictly increases on a join", ok);
        prev = swim.epoch();

        (void)swim.admit(NodeId{3}, kCluster);
        check(swim.epoch() > prev, "epoch strictly increases on a second join", ok);
        prev = swim.epoch();

        swim.leave(NodeId{2});  // graceful leave ⇒ roster shrinks
        check(swim.epoch() > prev, "epoch strictly increases on a leave/dead", ok);
        check(!swim.view().contains(NodeId{2}), "a departed node leaves the current view()", ok);

        // The pinned start snapshot is STILL valid to read and distinguishable as stale by its epoch.
        const MembershipView v_now = swim.view();
        check(v_start.epoch() < v_now.epoch(), "a stale snapshot has a strictly lower epoch", ok);
        check(!v_start.contains(NodeId{3}) && v_now.contains(NodeId{3}),
              "the stale snapshot reflects the OLD roster (no resurrection of the current view)", ok);
        check(v_start.size() < v_now.size() || v_start.epoch() != v_now.epoch(),
              "stale vs current snapshots are totally ordered by epoch", ok);
    }

    std::printf("cluster_identity_epoch_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
