// Tests 021-Cluster-Formation-and-Lifecycle §3 (indirect ping / ping-req — SWIM's core false-positive
// reducer). When a DIRECT ping to a peer is not acked, a node asks k relays to ping it before
// declaring suspicion. A peer that is merely unreachable *directly* (a partial partition) but reachable
// *via a relay* must NOT be suspected — the indirect ping rescues it.
//
//   Phase 1 (relay available): node A cannot reach B directly (one-way link filter A↛B), but a relay R
//     reaches B. A's direct ack misses → indirect ping via R succeeds → A keeps B Alive, never suspects.
//   Phase 2 (relay removed): with R offline there is no indirect path either, so the SAME direct
//     partition now (correctly) drives B to Suspect → Dead — proving the indirect ping was the thing
//     holding B alive in phase 1.
// Virtual clock only; seeded selection; no sleeping.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

constexpr ClusterId kCluster{0xBEEF};
constexpr std::int64_t kStep = 120'000'000;

SwimMembership::Config make_cfg() {
    SwimMembership::Config c;
    c.cluster_id = kCluster;
    c.ack_timeout_ns = 100'000'000;
    c.suspicion_timeout_ns = 400'000'000;
    c.indirect_k = 2;
    c.seed = 0x2222;
    return c;
}

struct Node {
    LoopbackTransport transport;
    SwimMembership swim;
    Node(NodeId id, LoopbackFabric& f, VClock& clk)
        : transport(f, id), swim(id, transport, make_cfg()) {
        swim.set_clock(&vclock_read, &clk);
    }
};

void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;

    LoopbackFabric fabric;
    VClock clk;
    std::vector<std::unique_ptr<Node>> nodes;
    const std::uint64_t ids[] = {1, 2, 3};  // A=1, B=2, R=3
    for (std::uint64_t id : ids) nodes.push_back(std::make_unique<Node>(NodeId{id}, fabric, clk));
    for (auto& n : nodes)
        for (std::uint64_t id : ids)
            if (NodeId{id} != n->swim.self()) (void)n->swim.admit(NodeId{id}, kCluster);

    Node* a = nodes[0].get();
    const NodeId b{2};

    // One-way partition: A can reach everyone EXCEPT B directly. Relay R (=3) reaches B normally.
    a->swim.set_link_reachable([](NodeId to) noexcept { return to != NodeId{2}; });

    // ---- Phase 1: indirect ping keeps B alive despite the direct partition. -----------------------
    bool ever_suspect = false;
    for (int round = 0; round < 60; ++round) {
        clk.now += kStep;
        for (auto& n : nodes) n->swim.tick();
        if (a->swim.status_of(b) != MemberStatus::Alive) ever_suspect = true;
    }
    check(!ever_suspect, "direct-partitioned-but-relay-reachable B is NEVER suspected (indirect ping)",
          ok);
    check(a->swim.status_of(b) == MemberStatus::Alive, "A holds B Alive via the relay", ok);
    check(a->swim.view().contains(b), "B stays in A's view() through the partial partition", ok);

    // ---- Phase 2: remove the relay; now the direct partition has no indirect path → B dies on A. ---
    nodes[2]->swim.set_online(false);  // R offline: no relay for A→B anymore
    bool b_dead = false;
    for (int round = 0; round < 120 && !b_dead; ++round) {
        clk.now += kStep;
        a->swim.tick();  // B still alive & ticking, but A can neither reach it nor relay to it
        nodes[1]->swim.tick();
        b_dead = a->swim.status_of(b) == MemberStatus::Dead;
    }
    check(b_dead, "with no relay available, the direct partition drives B to Dead on A", ok);
    check(!a->swim.view().contains(b), "dead B is excluded from A's view() once the relay is gone", ok);

    std::printf("cluster_swim_indirect_ping_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
