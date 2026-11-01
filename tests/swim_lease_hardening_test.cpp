// Tests ADR-071 claims C1b, C2c, C9 and ADR-069 claim C11b (red-team #47, findings M2, M3, N2): the
// interaction of suspension leases (ADR-071) with admission/eviction (ADR-069) in SwimMembership.
// Implements 021 §"Mobile member profile" as amended by ADR-071 "Red-team of the merged changes (#47)".
// Virtual clock + deferred fabric (tests/swim_deferred_fabric.hpp), no sleeping. Nodes 1..4: nodes 1 and 2
// run the application's AdmissionAuthority (authority_only_revocation, the consumer's mode); node 3 is
// LogExchangeOnly there (it can gossip, it is not placement-eligible); node 4 is the phone.
//
//   T1 (M2)  a peer gossips Suspect (and Dead) about a SUSPENDED member at lease incarnation +1: both lease
//            holders ignore it (no Suspect, no Dead, member stays in view, lease held). CONTROL: the old
//            incarnation-gated suppression kills the phone at both holders; the +0 variant never did.
//   T2 (M3)  suspend, process killed, restarted within the lease and Join: the old lease no longer shields
//            it, so a real crash is suspected on the normal timeout. CONTROL: old behaviour, > 60 s.
//   T3 (M3)  evict -> reap tombstone -> re-admit -> restart: same, via the tombstone-clears-lease fix; the
//            two fixes (tombstone clears, Join clears) are each sufficient (defence in depth) and the
//            control with both off shows the old +807 s.
//   T4 (M3)  an evicted member holds no lease and raises no Resumed while the snapshot denies it.
//   T5 (N2)  this node suspended when its application denies a peer: the eviction happens while suspended
//            and resume() sends nothing to the denied peer. CONTROL: eviction deferred, a frame is sent.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "quark/core/admission.hpp"
#include "quark/core/mobile_member.hpp"
#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0x0B11E};
constexpr std::int64_t MS = 1000000;
constexpr std::int64_t kMinute = 60000 * MS;
constexpr std::uint64_t kPhone = 4, kLogOnly = 3;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

using Faults = SwimMembership::Config::TestFaults;

SwimMembership::Config base_cfg(const Faults& f) {
    SwimMembership::Config c;
    c.cluster_id = kCluster;
    c.ack_timeout_ns = 100 * MS;
    c.suspicion_timeout_ns = 500 * MS;
    c.seed = 0xA5A5;
    c.authority_only_revocation = true;
    c.test_faults = f;
    return c;
}

struct Events {
    std::vector<MembershipEvent> v;
    int count(MembershipEventKind k, std::uint64_t node) const {
        int n = 0;
        for (const auto& e : v)
            if (e.kind == k && e.node.value == node) ++n;
        return n;
    }
};

std::shared_ptr<const AdmissionSnapshot> snap(std::uint64_t g, std::vector<std::uint64_t> admit,
                                              std::vector<std::uint64_t> log_only = {}) {
    AdmissionSnapshot::Builder b(g);
    for (auto a : admit) b.admit(NodeId{a});
    for (auto l : log_only) b.log_exchange_only(NodeId{l});
    return b.build();
}

struct World {
    AdmissionAuthority a1, a2;  // must outlive the memberships
    Sim sim;
    Events ev[5];
    explicit World(const Faults& f, bool admission = true) {
        sim.cfg = base_cfg(f);
        for (std::uint64_t i = 1; i <= 4; ++i) sim.add(i);
        if (admission) {
            sim.s(1).set_admission(a1);
            sim.s(2).set_admission(a2);
            (void)a1.publish(snap(1, {1, 2, 4}, {kLogOnly}));
            (void)a2.publish(snap(1, {1, 2, 4}, {kLogOnly}));
        }
        sim.admit_all();
        for (std::uint64_t i = 1; i <= 4; ++i)
            sim.s(i).add_event_sink([this, i](const MembershipEvent& e) { ev[i].v.push_back(e); });
        for (int t = 0; t < 30; ++t) sim.round();
    }
    void run_for(std::int64_t ns) {
        const std::int64_t end = sim.now() + ns;
        while (sim.now() < end) sim.round();
    }
    void deliver_now() { sim.fab.flush(16, 0, 0, INT64_MAX, sim.clk); }
    // An authenticated control frame FROM `from` (it speaks for itself; the digest is its choice).
    void inject_gossip(std::uint64_t from, std::uint64_t to, std::vector<MemberUpdate> updates) {
        ControlMsg m;
        m.kind = ControlKind::Gossip;
        m.cluster = kCluster;
        m.from = NodeId{from};
        m.from_incarnation = sim.s(from).self_incarnation();
        m.wire_version = kSwimWireVersion;
        m.updates = std::move(updates);
        MessageFrame f;
        f.from = NodeId{from};
        f.to = NodeId{to};
        f.kind = FrameKind::Control;
        f.payload = detail::encode_control(m);
        sim.fab.send(NodeId{from}, NodeId{to}, std::move(f));
    }
};

// T1: a log-only peer gossips `status` about the suspended, admitted phone at lease incarnation + `bump`.
// Returns true iff the phone was removed (Dead) at both lease holders.
bool t1_forged(const Faults& f, std::uint8_t status, int bump, bool& lease_held, std::uint64_t& suppressed) {
    World w(f);
    MobileMember phone(w.sim.s(kPhone), nullptr);
    phone.suspend(SuspendReason::AppBackground, 12 * kMinute);
    w.deliver_now();
    check(w.sim.s(1).is_suspended(NodeId{kPhone}) && w.sim.s(2).is_suspended(NodeId{kPhone}), "T1 setup: leases held");
    const std::uint64_t inc = w.sim.s(kPhone).self_incarnation();
    MemberUpdate u{};
    u.node = kPhone;
    u.incarnation = inc + static_cast<std::uint64_t>(bump);
    u.status = status;
    w.inject_gossip(kLogOnly, 1, {u});  // only node 1 is told; node 2 learns it by gossip
    w.run_for(5000 * MS);
    lease_held = w.sim.s(1).is_suspended(NodeId{kPhone}) && w.sim.s(2).is_suspended(NodeId{kPhone});
    suppressed = w.sim.s(1).suspicions_suppressed();
    return w.sim.s(1).status_of(NodeId{kPhone}) == MemberStatus::Dead &&
           w.sim.s(2).status_of(NodeId{kPhone}) == MemberStatus::Dead;
}

// First Suspect (ms after the crash) that node 1 raises for a phone that was restarted and then crashed
// for real. `evicted_first`: evict + reap + re-admit before the restart (T3) instead of a plain restart (T2).
std::int64_t restart_then_crash(const Faults& f, bool evicted_first) {
    World w(f, evicted_first);
    auto phone = std::make_unique<MobileMember>(w.sim.s(kPhone), nullptr);
    phone->suspend(SuspendReason::AppBackground, 15 * kMinute);
    w.deliver_now();
    if (evicted_first) {
        (void)w.a1.publish(snap(2, {1, 2}, {kLogOnly}));
        w.sim.s(1).apply_admission();
        w.sim.s(kPhone).set_online(false);
        w.run_for(90000 * MS);  // > tombstone TTL (60 s): node 1 forgets the tombstone
        (void)w.a1.publish(snap(3, {1, 2, 4}, {kLogOnly}));
    } else {
        w.run_for(30000 * MS);
    }
    phone.reset();
    w.sim.nodes.erase(kPhone);
    SwimMembership& fresh = w.sim.add(kPhone);  // restarted process, incarnation 0
    fresh.request_join(NodeId{1});
    w.deliver_now();
    for (int t = 0; t < 30; ++t) w.sim.round();
    check(w.sim.s(1).view().contains(NodeId{kPhone}), "restart: the phone is back in node 1's view");
    fresh.set_online(false);  // now it crashes for real
    const std::int64_t t0 = w.sim.now();
    const int before = w.ev[1].count(MembershipEventKind::Suspect, kPhone);
    while (w.sim.now() < t0 + 20 * kMinute) {
        w.sim.round();
        if (w.ev[1].count(MembershipEventKind::Suspect, kPhone) > before) return (w.sim.now() - t0) / MS;
    }
    return -1;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Faults none{};

    // ---- T1 (M2) ----
    for (const std::uint8_t status : {static_cast<std::uint8_t>(MemberStatus::Suspect), static_cast<std::uint8_t>(MemberStatus::Dead)}) {
        const char* name = status == static_cast<std::uint8_t>(MemberStatus::Suspect) ? "Suspect" : "Dead";
        bool held = false;
        std::uint64_t sup = 0;
        const bool killed = t1_forged(none, status, +1, held, sup);
        std::printf("T1 %s at lease inc +1: phone killed=%d lease_held=%d suppressed=%llu\n", name,
                    static_cast<int>(killed), static_cast<int>(held), static_cast<unsigned long long>(sup));
        check(!killed && held && sup > 0, "T1: second-hand Suspect/Dead at lease incarnation +1 is ignored while leased");
        Faults old{};
        old.incarnation_gated_suppression = true;
        const bool killed_old = t1_forged(old, status, +1, held, sup);
        std::printf("T1 CONTROL %s +1 (old gating): phone killed=%d\n", name, static_cast<int>(killed_old));
        check(killed_old, "T1 control: the old incarnation-gated suppression lets +1 kill the phone");
        const bool killed_zero = t1_forged(old, status, 0, held, sup);
        check(!killed_zero, "T1 control: +0 was always suppressed");
    }

    // ---- T2 (M3, restart within lease) ----
    {
        const std::int64_t fixed = restart_then_crash(none, false);
        Faults old{};
        old.join_keeps_lease = true;
        old.keep_lease_on_tombstone = true;
        const std::int64_t ctl = restart_then_crash(old, false);
        std::printf("T2 restart within lease, crash: first Suspect at +%lld ms (fixed), +%lld ms (CONTROL old)\n",
                    static_cast<long long>(fixed), static_cast<long long>(ctl));
        check(fixed > 0 && fixed < 2000, "T2: a crash after a restart is suspected on the normal timeout");
        check(ctl > 60000, "T2 control: the previous process's lease shielded the restart");
    }

    // ---- T3 (M3, evict -> reap -> re-admit -> restart) ----
    {
        Faults tomb_only_fix{};  // Join no longer clears (fault), tombstone clears
        tomb_only_fix.join_keeps_lease = true;
        Faults join_only_fix{};  // tombstone no longer clears (fault), Join clears
        join_only_fix.keep_lease_on_tombstone = true;
        Faults old{};
        old.join_keeps_lease = true;
        old.keep_lease_on_tombstone = true;
        const std::int64_t both = restart_then_crash(none, true);
        const std::int64_t a = restart_then_crash(tomb_only_fix, true);
        const std::int64_t b = restart_then_crash(join_only_fix, true);
        const std::int64_t ctl = restart_then_crash(old, true);
        std::printf("T3 evict/reap/re-admit/restart: first Suspect at +%lld (both fixes), +%lld (tombstone fix only), "
                    "+%lld (Join fix only), +%lld ms (CONTROL old)\n", static_cast<long long>(both),
                    static_cast<long long>(a), static_cast<long long>(b), static_cast<long long>(ctl));
        check(both > 0 && both < 2000, "T3: both fixes: normal detection");
        check(a > 0 && a < 2000, "T3: the tombstone fix alone suffices");
        check(b > 0 && b < 2000, "T3: the Join fix alone suffices");
        check(ctl > 60000, "T3 control: the old behaviour shields the restarted member for minutes");
    }

    // ---- T4 (M3, evicted member holds no lease, raises no Resumed) ----
    for (const bool old : {false, true}) {
        Faults f{};
        f.keep_lease_on_tombstone = old;
        World w(f);
        MobileMember phone(w.sim.s(kPhone), nullptr);
        phone.suspend(SuspendReason::Doze, 12 * kMinute);
        w.deliver_now();
        (void)w.a1.publish(snap(2, {1, 2}, {kLogOnly}));  // node 1's application denies the phone
        w.sim.s(1).apply_admission();
        check(w.ev[1].count(MembershipEventKind::Evicted, kPhone) == 1 && !w.sim.s(1).view().contains(NodeId{kPhone}),
              "T4 setup: phone evicted at node 1");
        const auto h = w.sim.s(1).health(NodeId{kPhone});
        const bool suspended = w.sim.s(1).is_suspended(NodeId{kPhone});
        const bool hs = h && h->suspended;
        phone.resume();  // the phone is not told; its frame reaches node 1's SWIM
        w.deliver_now();
        const int resumed = w.ev[1].count(MembershipEventKind::Resumed, kPhone);
        std::printf("T4%s: after eviction is_suspended=%d health.suspended=%d suspended_count=%zu; after resume Resumed=%d\n",
                    old ? " CONTROL (old)" : "", static_cast<int>(suspended), static_cast<int>(hs),
                    w.sim.s(1).suspended_count(), resumed);
        if (!old) {
            check(!suspended && !hs && w.sim.s(1).suspended_count() == 0, "T4: an evicted member holds no lease");
            check(resumed == 0, "T4: no Resumed while the snapshot denies the member");
        } else {
            check(suspended && resumed == 1, "T4 control: the old behaviour kept the lease and raised Resumed");
        }
    }

    // ---- T5 (N2) ----
    for (const bool old : {false, true}) {
        Faults f{};
        f.defer_eviction_while_suspended = old;
        AdmissionAuthority a4;
        World w(f, false);
        w.sim.s(kPhone).set_admission(a4);
        (void)a4.publish(snap(1, {1, 2, 3, 4}));
        w.sim.s(kPhone).apply_admission();
        MobileMember phone(w.sim.s(kPhone), nullptr);
        phone.suspend(SuspendReason::AppBackground, 12 * kMinute);
        w.deliver_now();
        (void)a4.publish(snap(2, {1, 3, 4}));  // deny node 2 while suspended
        w.run_for(10000 * MS);
        const std::uint64_t ev_while = w.sim.s(kPhone).evictions();
        int to_denied = 0;
        w.sim.fab.link = [&](NodeId a, NodeId b) {
            if (a.value == kPhone && b.value == 2) ++to_denied;
            return Link::Deliver;
        };
        phone.resume();
        w.deliver_now();
        std::printf("T5%s: evictions while suspended=%llu; frames to denied node 2 at resume=%d\n",
                    old ? " CONTROL (old)" : "", static_cast<unsigned long long>(ev_while), to_denied);
        if (!old) {
            check(ev_while == 1 && !w.sim.s(kPhone).view().contains(NodeId{2}), "T5: eviction applied while suspended");
            check(to_denied == 0, "T5: resume() sends nothing to the denied peer");
        } else {
            check(ev_while == 0 && to_denied > 0, "T5 control: old behaviour deferred the eviction and gossiped to the denied peer");
        }
    }

    std::printf(g_ok ? "swim_lease_hardening_test: PASS\n" : "swim_lease_hardening_test: FAIL\n");
    return g_ok ? 0 : 1;
}
