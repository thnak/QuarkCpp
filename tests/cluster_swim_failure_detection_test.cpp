// Tests 021-Cluster-Formation-and-Lifecycle §3 (SWIM failure detection) over a loopback multi-node
// cluster driven by an INJECTED VIRTUAL CLOCK — no sleeping, no wall-clock wait. Two invariants:
//   (A) DETECT + CONVERGE: a node whose acks stop (crash) goes Suspect, then Dead once the suspicion
//       timeout lapses with no refutation; every OTHER node's view() converges to exclude it.
//   (B) FALSE-POSITIVE GUARD: a node that RESUMES acking before the suspicion timeout is NEVER declared
//       dead — it refutes and converges back to Alive across the cluster.
// Determinism: peer/fanout selection is seeded splitmix (never wall time / random_device); the whole
// run is a pure function of the seed + the advance()/tick() sequence.
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

constexpr ClusterId kCluster{0xC0FFEE};
constexpr std::int64_t kAck = 100'000'000;         // 100 ms
constexpr std::int64_t kSuspicion = 500'000'000;   // 500 ms
constexpr std::int64_t kStep = 120'000'000;        // 120 ms per tick round (> ack ⇒ escalates in 1 tick)

SwimMembership::Config make_cfg() {
    SwimMembership::Config c;
    c.cluster_id = kCluster;
    c.ack_timeout_ns = kAck;
    c.suspicion_timeout_ns = kSuspicion;
    c.indirect_k = 2;
    c.gossip_fanout = 3;
    c.seed = 0xA5A5;
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

    // ---- Scenario A: crash a node → Suspect → Dead → every live node's view() excludes it. --------
    {
        LoopbackFabric fabric;
        VClock clk;
        std::vector<std::unique_ptr<Node>> nodes;
        const std::uint64_t ids[] = {1, 2, 3, 4};
        for (std::uint64_t id : ids) nodes.push_back(std::make_unique<Node>(NodeId{id}, fabric, clk));
        // Preload the full roster on every node (the post-join steady state SWIM then maintains).
        for (auto& n : nodes)
            for (std::uint64_t id : ids)
                if (NodeId{id} != n->swim.self()) (void)n->swim.admit(NodeId{id}, kCluster);

        const NodeId victim{4};
        Node* vnode = nodes[3].get();
        vnode->swim.set_online(false);  // crash: stops acking / gossiping (no transport detach needed)

        std::vector<Node*> live;
        for (auto& n : nodes)
            if (n->swim.self() != victim) live.push_back(n.get());

        // Pump virtual time + ticks. Detection must complete well within this budget.
        bool converged = false;
        bool saw_suspect = false;
        for (int round = 0; round < 200 && !converged; ++round) {
            clk.now += kStep;
            for (Node* n : live) n->swim.tick();
            if (live[0]->swim.status_of(victim) == MemberStatus::Suspect) saw_suspect = true;
            converged = true;
            for (Node* n : live)
                if (n->swim.view().contains(victim) || n->swim.status_of(victim) != MemberStatus::Dead)
                    converged = false;
        }
        check(saw_suspect, "victim passed through Suspect before Dead", ok);
        check(converged, "every live node converges to exclude the dead victim from view()", ok);
        for (Node* n : live) {
            check(!n->swim.view().contains(victim), "dead victim absent from a live node's view()", ok);
            check(n->swim.view().contains(n->swim.self()), "a live node still sees itself", ok);
        }
    }

    // ---- Scenario B: resume acking before the suspicion timeout → NEVER declared dead. -------------
    {
        LoopbackFabric fabric;
        VClock clk;
        std::vector<std::unique_ptr<Node>> nodes;
        const std::uint64_t ids[] = {1, 2, 3, 4};
        for (std::uint64_t id : ids) nodes.push_back(std::make_unique<Node>(NodeId{id}, fabric, clk));
        for (auto& n : nodes)
            for (std::uint64_t id : ids)
                if (NodeId{id} != n->swim.self()) (void)n->swim.admit(NodeId{id}, kCluster);

        const NodeId flapper{4};
        Node* fnode = nodes[3].get();
        std::vector<Node*> others;
        for (auto& n : nodes)
            if (n->swim.self() != flapper) others.push_back(n.get());

        fnode->swim.set_online(false);  // brief outage — long enough to be SUSPECTED, not to die.

        bool ever_dead = false;
        bool suspected = false;
        bool revived = false;
        bool back_alive = false;
        for (int round = 0; round < 200 && !back_alive; ++round) {
            clk.now += kStep;
            // Tick the flapper too once it is back online, so it can refute + gossip.
            for (auto& n : nodes)
                if (n->swim.online()) n->swim.tick();

            if (!suspected && others[0]->swim.status_of(flapper) == MemberStatus::Suspect)
                suspected = true;
            // As SOON as it is suspected, resume it — well before the suspicion timeout elapses.
            if (suspected && !revived) {
                fnode->swim.set_online(true);
                revived = true;
            }
            if (fnode->swim.status_of(flapper) == MemberStatus::Dead) ever_dead = true;
            for (Node* n : others)
                if (n->swim.status_of(flapper) == MemberStatus::Dead) ever_dead = true;

            if (revived) {
                back_alive = true;
                for (Node* n : others)
                    if (n->swim.status_of(flapper) != MemberStatus::Alive) back_alive = false;
            }
        }
        check(suspected, "flapper was genuinely suspected during its outage", ok);
        check(revived, "flapper resumed (test reached the revive point)", ok);
        check(!ever_dead, "a node that resumes before the suspicion timeout is NEVER declared dead", ok);
        check(back_alive, "the refuted flapper converges back to Alive on every other node", ok);
        for (Node* n : others)
            check(n->swim.view().contains(flapper), "flapper remains in every live node's view()", ok);
    }

    std::printf("cluster_swim_failure_detection_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
