// Tests ADR-069 / 021-Cluster-Formation §"Application-driven eviction" and 020-Security §1a authority-only
// revocation (Draft amendments):
//   T7  an admission snapshot that denies a live member evicts it: an `Evicted` membership event on EVERY
//       node that observes the generation, the node leaves view() and placement, and re-admission is refused
//       (gossip/join/probe) until a later snapshot admits it. Control: a generation that still admits it
//       evicts nothing.
//   T6  authority-only revocation: a revocation gossiped by an ordinary member is ignored (and not
//       re-gossiped); the DEFAULT mode applies it (control); the snapshot still denies in authority-only mode.
#include <cstdio>
#include <vector>

#include "admission_test_util.hpp"
#include "quark/core/cluster.hpp"
#include "quark/core/revocation_registry.hpp"

using namespace quark;
using namespace quark::test;

namespace {

struct VClock {
    std::int64_t now = 0;
};
std::int64_t vclock_read(void* ctx) noexcept { return static_cast<VClock*>(ctx)->now; }

SwimMembership::Config base_cfg(ClusterId cluster) {
    SwimMembership::Config cfg;
    cfg.cluster_id = cluster;
    cfg.ack_timeout_ns = 100'000'000;
    cfg.suspicion_timeout_ns = 500'000'000;
    cfg.gossip_fanout = 3;
    cfg.seed = 0x51D3;
    return cfg;
}

std::size_t count_kind(const std::vector<MembershipEvent>& ev, MembershipEventKind k, NodeId n) {
    std::size_t c = 0;
    for (const MembershipEvent& e : ev)
        if (e.kind == k && e.node == n) ++c;
    return c;
}

void t7_eviction(bool control, bool& ok) {
    const ClusterId cluster{777};
    const NodeId n1{1}, n2{2}, n3{3};
    AdmissionAuthority a1, a2;  // declared before the memberships: an authority must outlive them
    LoopbackFabric fabric;
    VClock clk;
    LoopbackTransport t1(fabric, n1), t2(fabric, n2), t3(fabric, n3);
    SwimMembership s1(n1, t1, base_cfg(cluster)), s2(n2, t2, base_cfg(cluster)), s3(n3, t3, base_cfg(cluster));
    for (SwimMembership* s : {&s1, &s2, &s3}) s->set_clock(&vclock_read, &clk);
    s1.set_admission(a1);
    s2.set_admission(a2);
    (void)a1.publish(snap(1, {n1, n2, n3}));
    (void)a2.publish(snap(1, {n1, n2, n3}));
    std::vector<MembershipEvent> ev1, ev2;
    s1.add_event_sink([&](const MembershipEvent& e) { ev1.push_back(e); });
    s2.add_event_sink([&](const MembershipEvent& e) { ev2.push_back(e); });
    for (SwimMembership* s : {&s1, &s2, &s3})
        for (NodeId n : {n1, n2, n3})
            if (n != s->self()) (void)s->admit(n, cluster);
    auto rounds = [&](int n) {
        for (int i = 0; i < n; ++i) {
            clk.now += 120'000'000;
            s1.tick();
            s2.tick();
            s3.tick();
        }
    };
    rounds(5);
    check(s1.view().contains(n3) && s2.view().contains(n3), "T7: n3 is a live member everywhere before the deny", ok);

    // Each observing node's application publishes its own generation 2. (Control: one that still admits n3.)
    (void)a1.publish(control ? snap(2, {n1, n2, n3}) : snap(2, {n1, n2}));
    (void)a2.publish(control ? snap(2, {n1, n2, n3}) : snap(2, {n1, n2}));
    s1.apply_admission();  // the driving thread; tick() does the same implicitly
    rounds(1);
    if (control) {
        check(count_kind(ev1, MembershipEventKind::Evicted, n3) == 0 && s1.view().contains(n3) &&
                  s1.evictions() == 0,
              "T7 CONTROL: a generation that still admits n3 evicts nothing", ok);
        return;
    }
    check(count_kind(ev1, MembershipEventKind::Evicted, n3) == 1, "T7: n1 reports exactly one Evicted event for n3", ok);
    check(count_kind(ev2, MembershipEventKind::Evicted, n3) == 1, "T7: n2 reports it too (every observing node)", ok);
    check(count_kind(ev1, MembershipEventKind::Dead, n3) == 0, "T7: the eviction is not conflated with a Dead event", ok);
    check(!s1.view().contains(n3) && !s2.view().contains(n3) && !s1.placement_view().contains(n3),
          "T7: n3 is out of the membership view and out of placement", ok);
    check(s1.status_of(n3) == MemberStatus::Dead, "T7: n3 is tombstoned", ok);
    const auto e = ev1.back();
    check(e.kind == MembershipEventKind::Evicted && e.epoch == s1.epoch(), "T7: the event carries the post-eviction epoch", ok);

    // n3 is not told and keeps probing/gossiping: every Alive update for it is refused.
    rounds(8);
    check(!s1.view().contains(n3) && s1.admission_refusals() > 0,
          "T7: gossip/probes from the evicted node cannot re-admit it", ok);
    check(s1.admit(n3, cluster) && !s1.view().contains(n3), "T7: an explicit join is refused while denied", ok);
    check(count_kind(ev1, MembershipEventKind::Joined, n3) == 1, "T7: no second Joined event while denied", ok);

    // A later snapshot admits it again: it rejoins by the ordinary higher-incarnation rule.
    (void)a1.publish(snap(3, {n1, n2, n3}));
    check(s1.admit(n3, cluster) && s1.view().contains(n3), "T7: re-admitted by a later generation", ok);
    check(count_kind(ev1, MembershipEventKind::Joined, n3) == 2, "T7: a fresh Joined event follows the re-admission", ok);
}

// A revocation gossiped by node 1 (an ordinary admitted member) about a THIRD party.
void t6_authority_only(bool authority_only, bool& ok) {
    const ClusterId cluster{778};
    AdmissionAuthority a2;  // declared before the membership that listens to it
    LoopbackFabric fabric;
    VClock clk;
    LoopbackTransport t1(fabric, NodeId{1}), t2(fabric, NodeId{2});
    SwimMembership::Config c2 = base_cfg(cluster);
    c2.authority_only_revocation = authority_only;
    SwimMembership s1(NodeId{1}, t1, base_cfg(cluster)), s2(NodeId{2}, t2, c2);
    s1.set_clock(&vclock_read, &clk);
    s2.set_clock(&vclock_read, &clk);
    (void)s1.admit(NodeId{2}, cluster);
    (void)s2.admit(NodeId{1}, cluster);
    RevocationRegistry reg1, reg2;
    reg2.set_authority_only(authority_only);
    s1.set_revocation_gossip([&](std::size_t m) { return reg1.sample(m); },
                             [&](const std::vector<Fingerprint>& in) { reg1.merge(in); });
    s2.set_revocation_gossip([&](std::size_t m) { return reg2.sample(m); },
                             [&](const std::vector<Fingerprint>& in) { reg2.merge(in); });
    const Fingerprint victim = fp_of(NodeId{13});  // a third party that node 1 wants gone
    reg1.revoke_locally(victim);
    bool reached = false;
    for (int i = 0; i < 40 && !reached; ++i) {
        clk.now += 120'000'000;
        s1.tick();
        s2.tick();
        reached = reg2.is_revoked(victim);
    }
    if (!authority_only) {
        check(reached, "T6 CONTROL (default mode): the gossiped third-party revocation takes effect", ok);
        return;
    }
    check(!reached && reg2.size() == 0, "T6: authority-only mode ignores the gossiped revocation", ok);
    check(s2.gossip_revocations_ignored() > 0, "T6: ...and counts what it ignored at the cluster", ok);
    // The registry-level switch is defence in depth: a direct merge is ignored and never re-gossiped.
    check(!reg2.merge({victim}) && reg2.ignored_gossip() == 1 && reg2.sample(10).empty(),
          "T6: RevocationRegistry authority-only: merge ignored, nothing offered for gossip", ok);
    reg2.revoke_locally(fp_of(NodeId{14}));  // this node's own operator is not a peer
    check(reg2.is_revoked(fp_of(NodeId{14})) && reg2.sample(10).empty(),
          "T6: a local operator revocation still applies but is not gossiped in this mode", ok);

    // The snapshot is the only source: denying node 1 still evicts it.
    s2.set_admission(a2);
    (void)a2.publish(snap(1, {NodeId{2}}));
    std::vector<MembershipEvent> ev;
    s2.add_event_sink([&](const MembershipEvent& e) { ev.push_back(e); });
    s2.apply_admission();
    check(count_kind(ev, MembershipEventKind::Evicted, NodeId{1}) == 1 && !s2.view().contains(NodeId{1}),
          "T6: in authority-only mode the snapshot still denies and evicts", ok);
}

}  // namespace

int main() {
    bool ok = true;
    t7_eviction(false, ok);
    t7_eviction(true, ok);
    t6_authority_only(true, ok);
    t6_authority_only(false, ok);
    std::printf("admission_membership_eviction_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
