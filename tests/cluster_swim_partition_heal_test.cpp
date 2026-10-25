// Tests ADR-049 (021 §1/§3): a SWIM cluster heals after a partition, a departed or restarted node can
// rejoin, and a genuinely crashed node is still detected Dead and is not resurrected by stale gossip.
// Runs on the deferred-delivery fabric (swim_deferred_fabric.hpp), never in the sender's call stack.
//
// Reproduces the 2026-10-21 run_swim_chaos.sh defect: on the pre-ADR-049 code, after the heal, the
// isolated node's stale "everyone is Dead" gossip was accepted by healthy nodes about EACH OTHER (equal
// incarnation, Dead terminal) and the cluster broke apart; and a Join for a Dead node was a no-op.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0xC4A05};
constexpr std::int64_t MS = 1'000'000;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

SwimMembership::Config base_cfg() {
    SwimMembership::Config c;
    c.cluster_id = kCluster;
    c.ack_timeout_ns = 100 * MS;
    c.suspicion_timeout_ns = 500 * MS;
    c.seed = 0xA5A5;
    return c;
}

// Partition the 5-node cluster into `group` (index = node id) for 10 s, heal, and require every view
// to be full again within 3 s and to stay full for 200 more ticks.
struct HealOutcome {
    bool partitioned = false;  // the cut really was observed (sanity)
    std::int64_t heal_ms = -1;
    bool stayed_full = true;
};
HealOutcome run_heal(const std::vector<int>& group, Link mode, std::uint64_t shuffle) {
    Sim sim;
    sim.cfg = base_cfg();
    sim.shuffle = shuffle;
    for (std::uint64_t i = 1; i <= 5; ++i) sim.add(i);
    sim.admit_all();
    for (int t = 0; t < 10; ++t) sim.round();

    sim.fab.link = [&](NodeId a, NodeId b) { return group[a.value] != group[b.value] ? mode : Link::Deliver; };
    const std::int64_t cut_end = sim.now() + 10'000 * MS;
    while (sim.now() < cut_end) sim.round();
    HealOutcome r;
    r.partitioned = sim.s(1).view().size() < 5;

    sim.fab.link = nullptr;
    sim.fab.release_held();
    const std::int64_t healed_at = sim.now();
    for (int t = 0; t < 400; ++t) {
        sim.round();
        if (r.heal_ms < 0) {
            if (sim.all_views_full(5)) r.heal_ms = (sim.now() - healed_at) / MS;
        } else if (!sim.all_views_full(5)) {
            r.stayed_full = false;
        }
        if (r.heal_ms >= 0 && t > 200 + r.heal_ms / 120) break;
    }
    return r;
}

void test_heal() {
    const std::vector<int> isolate5 = {0, 0, 0, 0, 0, 1};  // node 5 alone
    const std::vector<int> split = {0, 0, 0, 1, 1, 1};     // {1,2} | {3,4,5}
    std::int64_t worst = 0;
    for (const auto* g : {&isolate5, &split})
        for (Link mode : {Link::Drop, Link::Hold})
            for (std::uint64_t seed = 0; seed < 8; ++seed) {
                const HealOutcome r = run_heal(*g, mode, seed);
                check(r.partitioned, "the partition was observed before the heal");
                check(r.heal_ms >= 0 && r.heal_ms <= 3000, "every view is full again within 3 s of the heal");
                check(r.stayed_full, "views stay full after converging (no relapse)");
                if (r.heal_ms > worst) worst = r.heal_ms;
            }
    std::printf("  heal: isolate + split, drop + hold, 8 seeds each — worst %lld ms\n", static_cast<long long>(worst));
}

// A node that left gracefully is Left everywhere; a fresh process with the same NodeId rejoins via Join.
void test_leave_then_rejoin() {
    Sim sim;
    sim.cfg = base_cfg();
    for (std::uint64_t i = 1; i <= 4; ++i) sim.add(i);
    sim.admit_all();
    for (int t = 0; t < 10; ++t) sim.round();

    sim.s(4).leave(NodeId{4});
    for (int t = 0; t < 20; ++t) sim.round();
    check(sim.s(1).status_of(NodeId{4}) == MemberStatus::Left, "a graceful leave reaches node 1 as Left");
    check(sim.s(1).view().size() == 3, "the departed node is out of the view");

    // Restart node 4 as a fresh process (incarnation 1) and send a Join to node 1.
    sim.nodes[4] = std::make_unique<SwimMembership>(NodeId{4}, *sim.transports[4], sim.cfg);
    sim.s(4).set_clock(&vclock_read, &sim.clk);
    sim.s(4).request_join(NodeId{1});
    std::int64_t rejoined_ms = -1;
    for (int t = 0; t < 60 && rejoined_ms < 0; ++t) {
        sim.round();
        if (sim.all_views_full(4)) rejoined_ms = t * 120;
    }
    check(rejoined_ms >= 0, "a restarted node rejoins through Join after a Left tombstone");
    check(sim.s(4).self_incarnation() > SwimMembership::kLeftBand,
          "the rejoined node adopted an incarnation above its Left tombstone");
    // admit() takes the same path (in-process join gate).
    sim.s(2).leave(NodeId{3});
    check(sim.s(2).admit(NodeId{3}, kCluster) && sim.s(2).status_of(NodeId{3}) == MemberStatus::Alive,
          "admit() re-admits a node over its tombstone");
    std::printf("  leave → restart → rejoin in %lld ms\n", static_cast<long long>(rejoined_ms));
}

// A genuinely crashed node is still detected Dead, and stale gossip never resurrects it.
void test_crash_detected_not_resurrected() {
    Sim sim;
    sim.cfg = base_cfg();
    sim.d = 10 * MS;
    for (std::uint64_t i = 1; i <= 5; ++i) sim.add(i);
    sim.admit_all();
    for (int t = 0; t < 20; ++t) sim.round();
    sim.s(5).set_online(false);
    const std::int64_t crashed_at = sim.now();
    std::int64_t detected_ms = -1;
    for (int t = 0; t < 100 && detected_ms < 0; ++t) {
        sim.round();
        bool all = true;
        for (std::uint64_t i = 1; i <= 4; ++i) all = all && !sim.s(i).view().contains(NodeId{5});
        if (all) detected_ms = (sim.now() - crashed_at) / MS;
    }
    check(detected_ms >= 0 && detected_ms <= 3000, "every survivor drops the crashed node within 3 s");
    bool resurrected = false;
    for (int t = 0; t < 300; ++t) {
        sim.round();
        for (std::uint64_t i = 1; i <= 4; ++i) resurrected = resurrected || sim.s(i).view().contains(NodeId{5});
    }
    check(!resurrected, "the crashed node is never resurrected by gossip while its tombstone is resident");
    std::printf("  crash detected cluster-wide in %lld ms; not resurrected over 300 ticks\n",
                static_cast<long long>(detected_ms));
}

// The accidental-merge guard still holds for Join after the rule change.
void test_merge_guard() {
    Sim sim;
    sim.cfg = base_cfg();
    for (std::uint64_t i = 1; i <= 2; ++i) sim.add(i);
    sim.admit_all();
    check(!sim.s(1).admit(NodeId{9}, ClusterId{0xBAD}), "admit() rejects a foreign cluster id");
    check(!sim.s(1).is_known(NodeId{9}), "a rejected joiner is not absorbed");
}
}  // namespace

int main() {
    test_heal();
    test_leave_then_rejoin();
    test_crash_detected_not_resurrected();
    test_merge_guard();
    std::printf("cluster_swim_partition_heal_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
