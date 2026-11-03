// Tests ADR-082 (issue #54; assessment M12, SEC-16, SEC-17): SWIM gossip subjects are not authenticated (ADR-071 N4) and a
// member can cycle suspend leases. Implements 021 §"Failure detection" / §"Mobile member profile". Virtual clock + deferred fabric
// (tests/swim_deferred_fabric.hpp), no sleeping. Nodes 1..4; node 4 is the subject X, node 3 the LIAR (an admitted member that
// sends authenticated frames whose digest lies about X). Each property has a control that restores the old behaviour.
//
//   E1 (SEC-16) X crashes; the liar gossips `X Alive @ rising incarnation` every period: X is Dead at the honest peers within the
//               stated bound. CONTROL (unauthenticated_gossip_subjects): X is never Dead.
//   E2          `X Alive @ 1_000_000` is dropped (gossip_subject_rejected). CONTROL: X is recorded at 1_000_000.
//   E2b         a claim inside the jump bound is later LOWERED by X's own first-hand frame. CONTROL: it stays inflated.
//   E3          a Dead X plus a second-hand Alive at a higher incarnation becomes Suspect, then Dead again; it is not resurrected.
//               CONTROL: Alive at once.
//   E4          honest regression: X unreachable from node 1 only is not declared Dead there (indirect probes), with the fix on.
//   E5 (SEC-17) a 24 h refute-and-suspend loop: the member's granted lease time passes the alarm fraction and the alarm fires once;
//               with the alarm off nothing fires (control); with enforcement the granted time stays within the budget.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/mobile_member.hpp"
#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0x0B54};
constexpr std::int64_t MS = 1000000;
constexpr std::int64_t kMinute = 60000 * MS;
constexpr std::int64_t kHour = 60 * kMinute;
constexpr std::uint64_t kX = 4, kLiar = 3;

bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

using Faults = SwimMembership::Config::TestFaults;

struct World {
    Sim sim;
    std::vector<MembershipEvent> ev[5];
    explicit World(const Faults& f, const MobileProfile& mobile = {}, std::int64_t tick_ns = 120 * MS) {
        sim.cfg.cluster_id = kCluster;
        sim.cfg.ack_timeout_ns = 100 * MS;
        sim.cfg.suspicion_timeout_ns = 500 * MS;
        sim.cfg.seed = 0xA5A5;
        sim.cfg.test_faults = f;
        sim.cfg.mobile = mobile;
        sim.tick_ns = tick_ns;
        for (std::uint64_t i = 1; i <= 4; ++i) sim.add(i);
        sim.admit_all();
        for (std::uint64_t i = 1; i <= 4; ++i)
            sim.s(i).add_event_sink([this, i](const MembershipEvent& e) { ev[i].push_back(e); });
        for (int t = 0; t < 30; ++t) sim.round();
    }
    int count(std::uint64_t at, MembershipEventKind k, std::uint64_t node) const {
        int n = 0;
        for (const auto& e : ev[at])
            if (e.kind == k && e.node.value == node) ++n;
        return n;
    }
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
    void deliver_now() { sim.fab.flush(16, 0, 0, INT64_MAX, sim.clk); }
    static MemberUpdate claim(std::uint64_t node, std::uint64_t inc, MemberStatus st) {
        MemberUpdate u{};
        u.node = node;
        u.incarnation = inc;
        u.status = static_cast<std::uint8_t>(st);
        return u;
    }
};

// E1: X crashes, the liar claims `X Alive` at a rising incarnation to nodes 1 and 2 every period. Returns the virtual time (ms) after
// the crash at which X is Dead at BOTH honest peers, or -1 if it never is within `limit_ns`.
std::int64_t keep_alive_attack(const Faults& f, std::int64_t limit_ns) {
    World w(f);
    w.sim.s(kX).set_online(false);
    const std::int64_t t0 = w.sim.now();
    const std::uint64_t base = w.sim.s(kX).self_incarnation();
    std::uint64_t k = 0;
    while (w.sim.now() < t0 + limit_ns) {
        ++k;
        w.inject_gossip(kLiar, 1, {World::claim(kX, base + k, MemberStatus::Alive)});
        w.inject_gossip(kLiar, 2, {World::claim(kX, base + k, MemberStatus::Alive)});
        w.sim.round();
        if (w.sim.s(1).status_of(NodeId{kX}) == MemberStatus::Dead && w.sim.s(2).status_of(NodeId{kX}) == MemberStatus::Dead)
            return (w.sim.now() - t0) / MS;
    }
    return -1;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const Faults none{};
    Faults old{};
    old.unauthenticated_gossip_subjects = true;

    // ---- E1 (SEC-16) ----
    {
        // Stated bound: first Suspect within a few probe periods, one credit (one period), then one suspicion window. 6 s is generous.
        const std::int64_t fixed = keep_alive_attack(none, 8000 * MS);
        const std::int64_t ctrl = keep_alive_attack(old, 30000 * MS);
        std::printf("E1: X Dead after %lld ms with the fix; CONTROL (old): %lld ms (-1 = never within 30 s)\n",
                    static_cast<long long>(fixed), static_cast<long long>(ctrl));
        check(fixed >= 0 && fixed <= 6000, "E1: a crashed member held up by a lying member is Dead within the bound");
        check(ctrl == -1, "E1 control: the old behaviour keeps the crashed member alive indefinitely");
    }

    // ---- E2 / E2b ----
    for (const bool fixed : {true, false}) {
        World w(fixed ? none : old);
        const std::uint64_t real = w.sim.s(1).incarnation_of(NodeId{kX});
        w.inject_gossip(kLiar, 1, {World::claim(kX, 1'000'000, MemberStatus::Alive)});
        w.deliver_now();
        const std::uint64_t after = w.sim.s(1).incarnation_of(NodeId{kX});
        std::printf("E2 %s: X's incarnation at node 1: %llu -> %llu (rejected=%llu)\n", fixed ? "fix" : "CONTROL",
                    static_cast<unsigned long long>(real), static_cast<unsigned long long>(after),
                    static_cast<unsigned long long>(w.sim.s(1).gossip_subject_rejected()));
        if (fixed) check(after == real && w.sim.s(1).gossip_subject_rejected() >= 1, "E2: a claim far above the member's own word is dropped");
        else check(after == 1'000'000, "E2 control: the old behaviour records the poisoned incarnation");

        // E2b: a claim inside the bound is accepted, then corrected by X's own first-hand frame.
        World v(fixed ? none : old);
        const std::uint64_t r2 = v.sim.s(1).incarnation_of(NodeId{kX});
        v.inject_gossip(kLiar, 1, {World::claim(kX, r2 + 3, MemberStatus::Alive)});
        v.deliver_now();
        check(v.sim.s(1).incarnation_of(NodeId{kX}) == r2 + 3, "E2b setup: a claim inside the bound is recorded");
        for (int t = 0; t < 40; ++t) v.sim.round();  // X is alive and talks to node 1 (probes, acks, gossip)
        const std::uint64_t healed = v.sim.s(1).incarnation_of(NodeId{kX});
        std::printf("E2b %s: after X speaks first-hand the record is %llu (X's own %llu)\n", fixed ? "fix" : "CONTROL",
                    static_cast<unsigned long long>(healed), static_cast<unsigned long long>(v.sim.s(kX).self_incarnation()));
        if (fixed) check(healed == v.sim.s(kX).self_incarnation(), "E2b: X's own frame lowers the inflated record to its real incarnation");
        else check(healed == r2 + 3, "E2b control: the inflated record stays");
    }

    // ---- E3 ----
    for (const bool fixed : {true, false}) {
        World w(fixed ? none : old);
        w.sim.s(kX).set_online(false);
        for (int t = 0; t < 80; ++t) w.sim.round();  // Dead at node 1
        check(w.sim.s(1).status_of(NodeId{kX}) == MemberStatus::Dead, "E3 setup: X is Dead at node 1");
        const std::uint64_t inc = w.sim.s(1).incarnation_of(NodeId{kX});
        w.inject_gossip(kLiar, 1, {World::claim(kX, inc + 1, MemberStatus::Alive)});
        w.deliver_now();
        const MemberStatus right_after = w.sim.s(1).status_of(NodeId{kX});
        for (int t = 0; t < 80; ++t) w.sim.round();
        const MemberStatus later = w.sim.s(1).status_of(NodeId{kX});
        std::printf("E3 %s: right after the claim status=%d, 10 s later status=%d (2=Suspect/Dead per enum)\n",
                    fixed ? "fix" : "CONTROL", static_cast<int>(right_after), static_cast<int>(later));
        if (fixed) {
            check(right_after == MemberStatus::Suspect, "E3: a second-hand Alive over a tombstone becomes Suspect, not Alive");
            check(later == MemberStatus::Dead, "E3: the unverified member is Dead again (it never answered)");
        } else {
            check(right_after == MemberStatus::Alive, "E3 control: the old behaviour resurrects it to Alive");
        }
    }

    // ---- E4: honest regression ----
    {
        World w(none);
        w.sim.fab.link = [](NodeId a, NodeId b) {
            return (a.value == kX && b.value == 1) || (a.value == 1 && b.value == kX) ? Link::Drop : Link::Deliver;
        };
        for (int t = 0; t < 120; ++t) w.sim.round();  // 14 s
        check(w.sim.s(1).status_of(NodeId{kX}) != MemberStatus::Dead, "E4: X reachable through others is not Dead at node 1");
        check(w.sim.s(1).view().contains(NodeId{kX}), "E4: X stays in node 1's view");
    }

    // ---- E5 (SEC-17): lease cycling ----
    auto cycle = [&](const MobileProfile& prof, std::int64_t& granted, std::uint64_t& alarms, int& events,
                     std::int64_t hours) {
        World w(none, prof, 30000 * MS);  // 30 s protocol period keeps a 24 h virtual run cheap
        MobileMember phone(w.sim.s(kX), nullptr);
        std::int64_t t_end = w.sim.now() + hours * kHour;
        while (w.sim.now() < t_end) {
            phone.suspend(SuspendReason::Doze, 15 * kMinute);
            const std::int64_t until = w.sim.now() + 14 * kMinute;  // refute 1 minute before the lease would lapse
            while (w.sim.now() < until) w.sim.round();
            phone.resume();
            for (int t = 0; t < 2; ++t) w.sim.round();
            granted = std::max(granted, w.sim.s(1).lease_ns_granted_in_window(NodeId{kX}));  // peak: the window rolls at 24 h
        }
        alarms = w.sim.s(1).lease_alarms();
        events = w.count(1, MembershipEventKind::LeaseBudgetExceeded, kX);
        return w.sim.s(1).status_of(NodeId{kX}) != MemberStatus::Dead;
    };
    {
        std::int64_t granted = 0;
        std::uint64_t alarms = 0;
        int events = 0;
        MobileProfile prof;  // defaults: 24 h window, alarm at 0.75
        const bool alive = cycle(prof, granted, alarms, events, 24);
        std::printf("E5: 24 h cycling: peak granted-in-window %.1f h, alarms=%llu events=%d, X alive=%d\n",
                    static_cast<double>(granted) / static_cast<double>(kHour), static_cast<unsigned long long>(alarms), events,
                    static_cast<int>(alive));
        check(alive, "E5: the cycling member is never declared Dead (the stated bound: silent up to max_lease per proof of life)");
        check(granted > 18 * kHour, "E5: a refute-and-suspend loop spends most of the window under leases (the bound is real)");
        check(alarms == 1 && events == 1, "E5: the alarm fires exactly once per window");

        MobileProfile never = prof;
        never.lease_alarm_fraction = 2.0;  // control: alarm path disabled; the counter still shows the abuse
        std::int64_t g2 = 0;
        std::uint64_t a2 = 0;
        int e2 = 0;
        (void)cycle(never, g2, a2, e2, 24);
        check(a2 == 0 && e2 == 0 && g2 > 18 * kHour, "E5 control: without the alarm nothing is raised, the granted time is still counted");

        MobileProfile enforced = prof;
        enforced.lease_budget_fraction = 0.5;
        std::int64_t g3 = 0;
        std::uint64_t a3 = 0;
        int e3 = 0;
        (void)cycle(enforced, g3, a3, e3, 24);
        std::printf("E5 enforced at 0.5: granted-in-window %.1f h\n", static_cast<double>(g3) / static_cast<double>(kHour));
        check(g3 <= 12 * kHour + 15 * kMinute, "E5: with enforcement the granted lease time stays within the budget (plus one lease)");
    }

    std::printf("swim_gossip_subject_authority_test: %s\n", g_ok ? "OK" : "FAIL");
    return g_ok ? 0 : 1;
}
