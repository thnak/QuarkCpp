// Tests issue #20 / ADR-053 (021 §"Rolling upgrade"): SWIM wire versioning.
//   (1) codec: v1 stays byte-identical to the legacy layout; v2 = v1 + ONE trailing version byte; an
//       absent trailer decodes as v1 (so a legacy peer is "v1", and v1 decoders ignore the trailer).
//   (2) MIXED N / N-1 CLUSTER: a 5-node cluster whose nodes speak versions {N, N-1, N, N-1, N} runs the
//       same fault scenarios as tests/netem/run_swim_chaos.sh (form, isolate, split, asym, crash, leave)
//       on the deterministic deferred-delivery fabric and must behave exactly like a homogeneous one
//       with ZERO refusals. Done both for the real versions (N = kSwimWireVersion) and a synthetic N+1.
//   (3) N-2 IS REFUSED with a distinct, observable reason: counter, VersionRefused event carrying the
//       reason + the peer's version, sticky health() metadata, JoinReject(reason) back to a joiner -
//       and the refused peer's gossip is NEVER absorbed (it cannot poison the cluster).
// In-process simulated nodes at different wire versions (the Linux netns 5-process run is not
// available on this host - see ADR-053 "what was not exercised").
#include <algorithm>
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

// --- (1) codec -------------------------------------------------------------------------------------
void test_codec() {
    ControlMsg m;
    m.kind = ControlKind::Ping;
    m.cluster = kCluster;
    m.from = NodeId{7};
    m.from_incarnation = 3;
    m.seq = 42;
    m.updates.push_back(MemberUpdate{9, 2, 1});

    m.wire_version = 1;
    const auto v1 = detail::encode_control(m);
    m.wire_version = 2;
    const auto v2 = detail::encode_control(m);
    check(v2.size() == v1.size() + 1, "v2 is v1 plus exactly one trailer byte");
    check(std::equal(v1.begin(), v1.end(), v2.begin()), "v1 bytes are a strict prefix of v2 (a v1 decoder reads v2 unchanged)");
    check(static_cast<std::uint8_t>(v2.back()) == 2, "the trailer is the version byte");

    ControlMsg out;
    check(detail::decode_control(v1.data(), v1.size(), out) && out.wire_version == 1, "no trailer decodes as v1");
    check(detail::decode_control(v2.data(), v2.size(), out) && out.wire_version == 2 && out.seq == 42 &&
              out.updates.size() == 1,
          "v2 round-trips");
    // Pre-gate (legacy) encoder == v1 encoder: the legacy bytes are exactly what default-constructed
    // ControlMsg encodes to (wire_version defaults to 1).
    ControlMsg legacy = m;
    legacy.wire_version = 1;
    check(detail::encode_control(legacy) == v1, "default message is byte-identical to the legacy layout");
}

// --- (2) mixed cluster on the chaos scenarios ------------------------------------------------------
struct Mixed {
    Sim sim;
    explicit Mixed(const std::vector<std::uint8_t>& versions, std::uint64_t shuffle = 0) {
        sim.cfg = base_cfg();
        sim.shuffle = shuffle;
        for (std::size_t i = 0; i < versions.size(); ++i) {
            sim.cfg.wire_version = versions[i];
            sim.add(i + 1);
        }
        sim.admit_all();
    }
    [[nodiscard]] std::uint64_t refusals() {
        std::uint64_t r = 0;
        for (auto& [id, n] : sim.nodes) r += n->version_refusals();
        return r;
    }
};

// Partition `group` for 10 s, heal, require full views again within 3 s and no relapse.
bool heal_run(Mixed& mx, const std::vector<int>& group, Link mode, std::int64_t& heal_ms) {
    Sim& sim = mx.sim;
    for (int t = 0; t < 10; ++t) sim.round();
    sim.fab.link = [&](NodeId a, NodeId b) { return group[a.value] != group[b.value] ? mode : Link::Deliver; };
    const std::int64_t cut_end = sim.now() + 10'000 * MS;
    while (sim.now() < cut_end) sim.round();
    const bool partitioned = sim.s(1).view().size() < 5;
    sim.fab.link = nullptr;
    sim.fab.release_held();
    const std::int64_t healed_at = sim.now();
    heal_ms = -1;
    bool relapse = false;
    for (int t = 0; t < 400; ++t) {
        sim.round();
        if (heal_ms < 0) {
            if (sim.all_views_full(5)) heal_ms = (sim.now() - healed_at) / MS;
        } else if (!sim.all_views_full(5)) {
            relapse = true;
        }
        if (heal_ms >= 0 && t > 200 + heal_ms / 120) break;
    }
    return partitioned && heal_ms >= 0 && heal_ms <= 3000 && !relapse;
}

void run_mixed_scenarios(const std::vector<std::uint8_t>& vers, const char* label) {
    // form: roster preloaded, protocol keeps everyone alive and learns each peer's version.
    {
        Mixed mx(vers);
        for (int t = 0; t < 100; ++t) mx.sim.round();
        check(mx.sim.all_views_full(5), "mixed: form - every view full");
        check(mx.refusals() == 0, "mixed: form - zero refusals inside the window");
        bool learned = true;
        for (std::uint64_t a = 1; a <= 5; ++a)
            for (std::uint64_t b = 1; b <= 5; ++b)
                if (a != b) {
                    const auto h = mx.sim.s(a).health(NodeId{b});
                    learned = learned && h && h->protocol_version == vers[b - 1];
                }
        check(learned, "mixed: health() reports each peer's wire version");
    }
    // isolate + split (drop and hold), as run_swim_chaos.sh `isolate` / `split`.
    const std::vector<int> isolate5 = {0, 0, 0, 0, 0, 1};
    const std::vector<int> split = {0, 0, 0, 1, 1, 1};
    std::int64_t worst = 0;
    for (const auto* g : {&isolate5, &split})
        for (Link mode : {Link::Drop, Link::Hold})
            for (std::uint64_t seed = 0; seed < 4; ++seed) {
                Mixed mx(vers, seed);
                std::int64_t heal_ms = -1;
                check(heal_run(mx, *g, mode, heal_ms), "mixed: partition heals within 3 s, no relapse");
                check(mx.refusals() == 0, "mixed: partition run - zero refusals");
                if (heal_ms > worst) worst = heal_ms;
            }
    // asym: only the 1<->2 link cut; 3,4,5 relay indirect pings (a version-mixed relay path).
    {
        Mixed mx(vers);
        for (int t = 0; t < 10; ++t) mx.sim.round();
        mx.sim.fab.link = [](NodeId a, NodeId b) {
            const bool cut = (a.value == 1 && b.value == 2) || (a.value == 2 && b.value == 1);
            return cut ? Link::Drop : Link::Deliver;
        };
        for (int t = 0; t < 150; ++t) mx.sim.round();
        check(mx.sim.s(1).view().contains(NodeId{2}) && mx.sim.s(2).view().contains(NodeId{1}),
              "mixed: asym - 1 and 2 stay alive via indirect ping-req through mixed-version relays");
        check(mx.refusals() == 0, "mixed: asym - zero refusals");
    }
    // crash detection + graceful leave (a v(N) node leaves; v(N-1) peers must honour Left (status 3)).
    {
        Mixed mx(vers);
        mx.sim.d = 10 * MS;
        for (int t = 0; t < 20; ++t) mx.sim.round();
        mx.sim.s(5).set_online(false);
        bool all = false;
        for (int t = 0; t < 100 && !all; ++t) {
            mx.sim.round();
            all = true;
            for (std::uint64_t i = 1; i <= 4; ++i) all = all && !mx.sim.s(i).view().contains(NodeId{5});
        }
        check(all, "mixed: crash - every survivor (either version) drops the crashed node");
        mx.sim.s(3).leave(NodeId{3});
        for (int t = 0; t < 30; ++t) mx.sim.round();
        bool left = true;
        for (std::uint64_t i : {1ull, 2ull, 4ull}) left = left && mx.sim.s(i).status_of(NodeId{3}) == MemberStatus::Left;
        check(left, "mixed: leave - Left reaches both N and N-1 peers");
        check(mx.refusals() == 0, "mixed: crash/leave - zero refusals");
    }
    std::printf("  mixed %s: form, isolate+split (drop+hold x4 seeds, worst heal %lld ms), asym, crash, leave - OK\n",
                label, static_cast<long long>(worst));
}

// --- (3) N-2 refusal -------------------------------------------------------------------------------
void test_n_minus_2_refused() {
    constexpr std::uint8_t N = 3;
    // Nodes 1..4 speak N=3; node 5 speaks 1 (= N-2). Node 6 (v1) will try to Join through seed 1.
    Mixed mx({N, N, N, N, 1});
    Sim& sim = mx.sim;
    std::vector<MembershipEvent> ev1;
    sim.s(1).add_event_sink([&](const MembershipEvent& e) { ev1.push_back(e); });
    std::vector<MembershipEvent> ev5;
    sim.s(5).add_event_sink([&](const MembershipEvent& e) { ev5.push_back(e); });
    std::vector<MembershipEvent> ev2;
    sim.s(2).add_event_sink([&](const MembershipEvent& e) { ev2.push_back(e); });

    for (int t = 0; t < 120; ++t) sim.round();

    bool refused_ev = false;
    for (const auto& e : ev1)
        if (e.kind == MembershipEventKind::VersionRefused && e.node == NodeId{5} &&
            e.refusal == RefusalReason::WireVersionTooOld && e.protocol_version == 1 && !e.refused_by_peer)
            refused_ev = true;
    check(refused_ev, "N node reports VersionRefused(WireVersionTooOld, peer v1) for the N-2 node");
    check(sim.s(1).version_refusals() > 0, "refusals are counted");
    const auto h = sim.s(1).health(NodeId{5});
    check(h && h->last_refusal == RefusalReason::WireVersionTooOld && h->refused_version == 1,
          "health(): sticky refusal reason + the version it presented");

    bool new_ev = false;
    for (const auto& e : ev5)
        if (e.kind == MembershipEventKind::VersionRefused && e.refusal == RefusalReason::WireVersionTooNew &&
            e.protocol_version == N)
            new_ev = true;
    check(new_ev, "the N-2 node reports WireVersionTooNew for the N peers (distinct from TooOld)");

    // To the protocol the N-2 node behaves like a partitioned one: it is declared Dead by the cluster,
    // but WHY stays observable (distinct from a plain timeout), and nothing it said was absorbed:
    // node 5 suspected every N node, yet no N node ever heard a Suspect/Dead about another N node.
    check(!sim.s(1).view().contains(NodeId{5}), "the refused node ends up out of the N node's view (partition-like)");
    check(sim.s(1).health(NodeId{5})->last_refusal == RefusalReason::WireVersionTooOld, "...and the reason outlives the tombstone");
    bool poisoned = false;
    for (const auto& e : ev2)
        if ((e.kind == MembershipEventKind::Suspect || e.kind == MembershipEventKind::Dead) && e.node != NodeId{5})
            poisoned = true;
    check(!poisoned, "the refused node's gossip never reaches the N cluster's state");
    check(sim.s(1).view().size() == 4 && sim.s(2).view().size() == 4, "the four N nodes remain a healthy cluster");

    // Joiner path: a v1 node asks seed 1 (v3) to admit it; the seed answers JoinReject(reason).
    sim.cfg.wire_version = 1;
    sim.add(6);
    std::vector<MembershipEvent> ev6;
    sim.s(6).add_event_sink([&](const MembershipEvent& e) { ev6.push_back(e); });
    sim.s(6).request_join(NodeId{1});
    for (int t = 0; t < 10; ++t) sim.round();
    bool rejected = false;
    for (const auto& e : ev6)
        if (e.kind == MembershipEventKind::VersionRefused && e.refused_by_peer &&
            e.refusal == RefusalReason::WireVersionTooOld && e.node == NodeId{1})
            rejected = true;
    check(rejected, "a refused joiner is told why: VersionRefused(refused_by_peer, TooOld) from the seed");
    check(!sim.s(1).view().contains(NodeId{6}), "the refused joiner was not admitted");

    // Boundary: exactly N-1 and N+1 are accepted, N-2 / N+2 are not (pure verdict, both directions).
    for (std::uint8_t self : {std::uint8_t{3}, std::uint8_t{5}})
        for (int peer = 1; peer <= 7; ++peer) {
            Sim s2;
            s2.cfg = base_cfg();
            s2.cfg.wire_version = self;
            s2.add(1);
            s2.cfg.wire_version = static_cast<std::uint8_t>(peer);
            s2.add(2);
            s2.admit_all();
            for (int t = 0; t < 30; ++t) s2.round();
            const bool expect_ok = peer >= self - 1 && peer <= self + 1;
            const bool got_ok = s2.s(1).version_refusals() == 0 && s2.s(2).version_refusals() == 0;
            check(got_ok == expect_ok, "window is exactly [self-1, self+1]");
        }
    std::printf("  N-2: refused with reason (counter, event, health, JoinReject); window boundary verified\n");
}

}  // namespace

int main() {
    test_codec();
    run_mixed_scenarios({kSwimWireVersion, kSwimWireVersion - 1, kSwimWireVersion, kSwimWireVersion - 1, kSwimWireVersion},
                        "N=real(2)/N-1=legacy(1)");
    run_mixed_scenarios({4, 3, 4, 3, 4}, "N=4/N-1=3 (synthetic future)");
    test_n_minus_2_refused();
    std::printf("cluster_swim_wire_version_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
