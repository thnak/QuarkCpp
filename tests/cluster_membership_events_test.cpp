// Tests issue #21 / ADR-053 (010 + 021): membership events and per-node health metadata.
//   * a subscriber receives Joined / Suspect / Dead / Left (+ Recovered) with the roster epoch;
//   * the change-to-callback latency is bounded: 0 ticks for anything the subscribing node itself
//     observes (the callback runs inside the tick / frame / leave that caused it), and the detection
//     bound for a REMOTE crash is stated and asserted against the injected virtual clock;
//   * health(): protocol version + state per node; callbacks may re-enter the membership.
// Virtual clock only (deterministic) - no sleeping, no threads.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/cluster.hpp"
#include "quark/core/membership.hpp"
#include "quark/core/transport.hpp"

using namespace quark;

namespace {

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

constexpr ClusterId kCluster{0xE7E27};
constexpr std::int64_t kAck = 100'000'000;
constexpr std::int64_t kSuspicion = 500'000'000;
constexpr std::int64_t kStep = 120'000'000;  // protocol period P (> ack: a missed probe escalates per tick)

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
    Node(NodeId id, LoopbackFabric& f, VClock& clk) : transport(f, id), swim(id, transport, make_cfg()) {
        swim.set_clock(&vclock_read, &clk);
    }
};

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

struct Cluster {
    LoopbackFabric fabric;
    VClock clk;
    std::vector<std::unique_ptr<Node>> nodes;
    explicit Cluster(int n) {
        for (int i = 1; i <= n; ++i) nodes.push_back(std::make_unique<Node>(NodeId{std::uint64_t(i)}, fabric, clk));
    }
    Node& at(std::uint64_t id) { return *nodes[id - 1]; }
    void admit_all_after_subscribe() {
        for (auto& n : nodes)
            for (auto& m : nodes)
                if (n.get() != m.get()) (void)n->swim.admit(m->swim.self(), kCluster);
    }
    void tick_all() {
        clk.now += kStep;
        for (auto& n : nodes)
            if (n->swim.online()) n->swim.tick();
    }
};

const MembershipEvent* find(const std::vector<MembershipEvent>& v, MembershipEventKind k, NodeId n) {
    for (const auto& e : v)
        if (e.kind == k && e.node == n) return &e;
    return nullptr;
}

// --- A: Joined (with epoch) on admit; Suspect then Dead on a crash within the stated bound. -------
void test_crash_events_and_latency_bound() {
    Cluster c(4);
    std::vector<MembershipEvent> ev;            // node 1's subscriber
    std::vector<std::int64_t> cb_clock;         // virtual clock at callback time
    c.at(1).swim.add_event_sink([&](const MembershipEvent& e) {
        ev.push_back(e);
        cb_clock.push_back(c.clk.now);
    });
    c.admit_all_after_subscribe();

    for (std::uint64_t id : {2ull, 3ull, 4ull}) {
        const auto* j = find(ev, MembershipEventKind::Joined, NodeId{id});
        check(j != nullptr, "admit() delivers Joined for each peer");
        if (j) {
            check(j->epoch > 0, "Joined carries an epoch");
            check(j->protocol_version == 0 || j->protocol_version == kSwimWireVersion,
                  "Joined metadata: version unknown (admit) or current");
        }
    }
    // Epochs of Joined events strictly increase and the last equals the published epoch.
    std::uint64_t prev = 0;
    bool mono = true;
    for (const auto& e : ev) {
        if (e.kind != MembershipEventKind::Joined) continue;
        mono = mono && e.epoch > prev;
        prev = e.epoch;
    }
    check(mono, "Joined events carry strictly increasing epochs");
    check(prev == c.at(1).swim.view().epoch(), "the last Joined epoch equals the published view epoch");

    // Warm up so everyone has heard from everyone (protocol_version learned from live frames).
    for (int i = 0; i < 20; ++i) c.tick_all();
    for (std::uint64_t id : {2ull, 3ull, 4ull}) {
        const auto h = c.at(1).swim.health(NodeId{id});
        check(h && h->state == MemberStatus::Alive && h->protocol_version == kSwimWireVersion && h->verified,
              "health(): Alive, current protocol version, verified");
    }

    ev.clear();
    cb_clock.clear();
    c.at(4).swim.set_online(false);
    const std::int64_t crashed_at = c.clk.now;
    for (int i = 0; i < 200 && !find(ev, MembershipEventKind::Dead, NodeId{4}); ++i) c.tick_all();

    const auto* s = find(ev, MembershipEventKind::Suspect, NodeId{4});
    const auto* d = find(ev, MembershipEventKind::Dead, NodeId{4});
    check(s && d, "a crash produces Suspect then Dead for the subscriber");
    if (!(s && d)) return;
    check(s < d, "Suspect is delivered before Dead");
    check(d->epoch > s->epoch, "Dead bumps the roster epoch (Suspect does not shrink the view)");
    check(d->epoch == c.at(1).swim.view().epoch(), "the Dead event's epoch is the view epoch after the change");
    check(!c.at(1).swim.view().contains(NodeId{4}), "Dead node is out of view()");

    // CHANGE-TO-CALLBACK LATENCY = 0 ticks: the callback ran inside the very tick that changed state,
    // so the virtual clock at callback time equals the event's own transition time.
    for (std::size_t i = 0; i < ev.size(); ++i) check(cb_clock[i] == ev[i].at_ns, "callback runs at the transition's own instant");

    // DETECTION BOUNDS (n = 4, P = kStep): a peer is probed once per round-robin pass of <= n-1 periods;
    // a dead target then needs the direct window + the indirect window + one period of tick granularity
    // per escalation. Suspect <= (n-1)P + ack + indirect + 2P; Dead <= Suspect-bound + suspicion + P.
    const std::int64_t n = 4;
    const std::int64_t suspect_bound = (n - 1) * kStep + kAck + kAck + 2 * kStep;
    const std::int64_t dead_bound = suspect_bound + kSuspicion + kStep;
    check(s->at_ns - crashed_at <= suspect_bound, "Suspect delivered within the stated bound");
    check(d->at_ns - crashed_at <= dead_bound, "Dead delivered within the stated bound");
    check(d->at_ns - s->at_ns >= kSuspicion, "Dead is not declared before the suspicion timeout");
    std::printf("  crash: Suspect +%lld ms (bound %lld), Dead +%lld ms (bound %lld)\n",
                static_cast<long long>((s->at_ns - crashed_at) / 1'000'000), static_cast<long long>(suspect_bound / 1'000'000),
                static_cast<long long>((d->at_ns - crashed_at) / 1'000'000), static_cast<long long>(dead_bound / 1'000'000));

    const auto h = c.at(1).swim.health(NodeId{4});
    check(h && h->state == MemberStatus::Dead, "health(): state Dead after the event");
    check(c.at(1).swim.health_snapshot().size() == 4, "health_snapshot lists every known node (tombstone included)");
}

// --- B: Left - synchronous for a local leave(), and propagated (gossip) for a self-leave. ---------
void test_left_events() {
    Cluster c(3);
    std::vector<MembershipEvent> ev1, ev3;
    c.at(1).swim.add_event_sink([&](const MembershipEvent& e) { ev1.push_back(e); });
    c.at(3).swim.add_event_sink([&](const MembershipEvent& e) { ev3.push_back(e); });
    c.admit_all_after_subscribe();
    for (int i = 0; i < 10; ++i) c.tick_all();

    ev3.clear();
    c.at(3).swim.leave(NodeId{3});  // self-leave: the leaver's own subscriber hears Left before return
    const auto* self_left = find(ev3, MembershipEventKind::Left, NodeId{3});
    check(self_left != nullptr, "self leave() delivers Left synchronously (latency 0)");
    if (self_left) check(self_left->incarnation >= SwimMembership::kLeftBand, "Left carries the Left-band incarnation");

    ev1.clear();
    std::int64_t waited = 0;
    for (int i = 0; i < 50 && !find(ev1, MembershipEventKind::Left, NodeId{3}); ++i) {
        c.tick_all();
        waited += kStep;
    }
    check(find(ev1, MembershipEventKind::Left, NodeId{3}) != nullptr, "a peer's subscriber hears the Left via gossip");
    check(waited <= 5 * kStep, "Left propagates to a peer within 5 protocol periods (3-node loopback)");
    check(c.at(1).swim.health(NodeId{3})->state == MemberStatus::Left, "health(): state Left");

    ev1.clear();
    c.at(1).swim.leave(NodeId{2});  // local tombstone of a peer: synchronous too
    check(find(ev1, MembershipEventKind::Left, NodeId{2}) != nullptr, "leave(peer) delivers Left synchronously");
}

// --- C: Recovered after a false suspicion; never Dead. ------------------------------------------
void test_recovered() {
    Cluster c(4);
    std::vector<MembershipEvent> ev;
    c.at(1).swim.add_event_sink([&](const MembershipEvent& e) { ev.push_back(e); });
    c.admit_all_after_subscribe();
    for (int i = 0; i < 10; ++i) c.tick_all();
    ev.clear();

    Node& flapper = c.at(4);
    flapper.swim.set_online(false);
    bool revived = false;
    for (int i = 0; i < 100; ++i) {
        c.tick_all();
        if (!revived && find(ev, MembershipEventKind::Suspect, NodeId{4})) {
            flapper.swim.set_online(true);
            revived = true;
        }
        if (find(ev, MembershipEventKind::Recovered, NodeId{4})) break;
    }
    const auto* r = find(ev, MembershipEventKind::Recovered, NodeId{4});
    check(r != nullptr, "a refuted suspicion produces Recovered");
    check(find(ev, MembershipEventKind::Dead, NodeId{4}) == nullptr, "no Dead for a node that resumed in time");
    if (r) check(r->incarnation > 1, "Recovered carries the refuting (higher) incarnation");
}

// --- D: callbacks may re-enter the membership (health, leave) without corrupting delivery order. ---
void test_reentrancy() {
    Cluster c(3);
    std::vector<MembershipEvent> ev;
    bool inner_ok = true;
    c.at(1).swim.add_event_sink([&](const MembershipEvent& e) {
        ev.push_back(e);
        if (e.kind == MembershipEventKind::Joined && e.node == NodeId{3}) {
            inner_ok = inner_ok && c.at(1).swim.health(NodeId{3}).has_value();
            c.at(1).swim.leave(NodeId{2});  // re-entrant mutation from inside a callback
        }
    });
    (void)c.at(1).swim.admit(NodeId{2}, kCluster);
    (void)c.at(1).swim.admit(NodeId{3}, kCluster);
    check(inner_ok, "health() is callable from a callback");
    check(find(ev, MembershipEventKind::Joined, NodeId{2}) != nullptr, "Joined(2) delivered");
    check(find(ev, MembershipEventKind::Joined, NodeId{3}) != nullptr, "Joined(3) delivered");
    check(find(ev, MembershipEventKind::Left, NodeId{2}) != nullptr, "the re-entrant leave's Left is delivered (queued, not lost)");
}

// --- E: the std-only seam double also speaks the contract (Joined / Left, health). ---------------
void test_in_process_membership() {
    InProcessMembership m(NodeId{1});
    std::vector<MembershipEvent> ev;
    m.add_event_sink([&](const MembershipEvent& e) { ev.push_back(e); });
    m.join(NodeId{2});
    m.leave(NodeId{2});
    check(ev.size() == 2 && ev[0].kind == MembershipEventKind::Joined && ev[1].kind == MembershipEventKind::Left,
          "InProcessMembership: Joined then Left");
    check(ev.size() == 2 && ev[1].epoch > ev[0].epoch, "InProcessMembership: epochs advance");
    check(!m.health(NodeId{2}).has_value() && m.health(NodeId{1}).has_value(), "InProcessMembership health()");
}

}  // namespace

int main() {
    test_crash_events_and_latency_bound();
    test_left_events();
    test_recovered();
    test_reentrancy();
    test_in_process_membership();
    std::printf("cluster_membership_events_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
