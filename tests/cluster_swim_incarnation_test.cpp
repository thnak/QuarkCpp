// Tests 021-Cluster-Formation-and-Lifecycle §3 (incarnation numbers + refutation). A node that is
// FALSELY suspected refutes the rumor by re-broadcasting itself Alive with a strictly HIGHER
// incarnation; higher incarnation wins, so the false Suspect is overridden and the node stays Alive
// across the whole cluster — it is never declared dead by a stale suspicion.
//
// We inject a false Suspect(B) rumor as a raw Control/Gossip frame (the std-only control codec, the
// same one a real gossip carries), let it take hold on a peer, then pump the seeded protocol and
// assert the cluster converges to Alive at the refuted (higher) incarnation. Virtual clock only.
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

constexpr ClusterId kCluster{0x1234};

SwimMembership::Config make_cfg() {
    SwimMembership::Config c;
    c.cluster_id = kCluster;
    c.ack_timeout_ns = 100'000'000;
    c.suspicion_timeout_ns = 500'000'000;
    c.seed = 0x77;
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

// Inject a raw gossip rumor carrying `updates` toward node `to`, as if from node `from` — the exact
// framing a real SWIM gossip uses (Control frame + the std-only control codec).
void inject_gossip(LoopbackFabric& fabric, NodeId from, NodeId to, std::vector<MemberUpdate> updates) {
    ControlMsg m;
    m.kind = ControlKind::Gossip;
    m.cluster = kCluster;
    m.from = from;
    m.from_incarnation = 1;
    m.updates = std::move(updates);
    MessageFrame f;
    f.from = from;
    f.to = to;
    f.kind = FrameKind::Control;
    f.payload = detail::encode_control(m);
    fabric.send(to, std::move(f));
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

    LoopbackFabric fabric;
    VClock clk;
    std::vector<std::unique_ptr<Node>> nodes;
    const std::uint64_t ids[] = {1, 2, 3};
    for (std::uint64_t id : ids) nodes.push_back(std::make_unique<Node>(NodeId{id}, fabric, clk));
    for (auto& n : nodes)
        for (std::uint64_t id : ids)
            if (NodeId{id} != n->swim.self()) (void)n->swim.admit(NodeId{id}, kCluster);

    const NodeId subject{2};  // B — the node that will be falsely suspected
    Node* a = nodes[0].get();  // node 1 receives the rumor
    Node* b = nodes[1].get();
    Node* c = nodes[2].get();
    const std::uint64_t b_inc_before = b->swim.self_incarnation();

    // Inject "B is Suspect at incarnation 1" onto node A (a false rumor — B is perfectly healthy).
    inject_gossip(fabric, NodeId{3}, NodeId{1}, {MemberUpdate{subject.value, 1, /*Suspect*/ 1}});
    check(a->swim.status_of(subject) == MemberStatus::Suspect,
          "the false Suspect(B) rumor took hold on node A", ok);

    // Pump the seeded protocol: A gossips the suspicion to B, B refutes with a higher incarnation, the
    // Alive(B, higher) supersedes the Suspect everywhere.
    constexpr std::int64_t kStep = 120'000'000;
    bool ever_dead = false;
    bool converged = false;
    for (int round = 0; round < 100 && !converged; ++round) {
        clk.now += kStep;
        for (auto& n : nodes) n->swim.tick();
        for (auto& n : nodes)
            if (n->swim.status_of(subject) == MemberStatus::Dead) ever_dead = true;
        converged = a->swim.status_of(subject) == MemberStatus::Alive &&
                    b->swim.status_of(subject) == MemberStatus::Alive &&
                    c->swim.status_of(subject) == MemberStatus::Alive;
    }

    check(!ever_dead, "a refuted node is never declared dead by the stale suspicion", ok);
    check(converged, "the cluster converges to B Alive after refutation", ok);

    const std::uint64_t b_inc_after = b->swim.self_incarnation();
    check(b_inc_after > b_inc_before, "B refuted with a STRICTLY higher incarnation (higher wins)", ok);
    check(a->swim.incarnation_of(subject) == b_inc_after,
          "node A observes B at the refuted (higher) incarnation", ok);
    check(c->swim.incarnation_of(subject) == b_inc_after,
          "node C observes B at the refuted (higher) incarnation", ok);
    check(a->swim.view().contains(subject) && c->swim.view().contains(subject),
          "B stays in every peer's view() throughout the false suspicion", ok);

    std::printf("cluster_swim_incarnation_test: %s  (B inc %llu -> %llu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(b_inc_before),
                static_cast<unsigned long long>(b_inc_after));
    return ok ? 0 : 1;
}
