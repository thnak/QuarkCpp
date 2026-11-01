// Tests ADR-071 claims C1 (a graceful suspend is not suspected), C3 (resume rejoins immediately, no churn),
// C4 (a peer that missed the announcement still heals on resume) and C5 (a stale or forged suspend
// is ignored), per 021 §"Mobile member profile". Implements 019 §"Android target" lifecycle contract.
// Runs on the deferred-delivery fabric with a virtual clock: a "10 minute background" is 5000 protocol
// periods of virtual time, no sleeping.
//
// Each claim has a CONTROL that must show the opposite when the mechanism is absent:
//   C1 control: the SAME 10-minute silence WITHOUT suspend() (set_online(false)) is suspected then declared
//               Dead by every peer, proving the test would catch a suspicion storm.
//   C5 control: the same Suspend frame at the CURRENT incarnation is accepted.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "quark/core/mobile_member.hpp"
#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0x0B11E};
constexpr std::int64_t MS = 1'000'000;
constexpr std::int64_t kMinute = 60'000 * MS;

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
    c.tombstone_ttl_ns = 60 * kMinute;  // keep tombstones visible for the whole run
    return c;
}

struct Events {
    std::vector<MembershipEvent> v;
    [[nodiscard]] int count(MembershipEventKind k, std::uint64_t node) const {
        int n = 0;
        for (const auto& e : v)
            if (e.kind == k && e.node.value == node) ++n;
        return n;
    }
    [[nodiscard]] int count_any(MembershipEventKind k) const {
        int n = 0;
        for (const auto& e : v)
            if (e.kind == k) ++n;
        return n;
    }
};

constexpr std::uint64_t kPhone = 4;

struct World {
    Sim sim;
    Events ev[5];  // index = node id (1..3 peers, 4 phone)
    World() {
        sim.cfg = base_cfg();
        for (std::uint64_t i = 1; i <= 4; ++i) sim.add(i);
        sim.admit_all();
        for (std::uint64_t i = 1; i <= 4; ++i)
            sim.s(i).add_event_sink([this, i](const MembershipEvent& e) { ev[i].v.push_back(e); });
        for (int t = 0; t < 30; ++t) sim.round();  // settle: every node has heard from every other
    }
    void run_for(std::int64_t ns) {
        const std::int64_t end = sim.now() + ns;
        while (sim.now() < end) sim.round();
    }
    void deliver_now() { sim.fab.flush(16, 0, 0, INT64_MAX, sim.clk); }
};

}  // namespace

int main() {
    // ---- C1 + C3: a phone suspended for 10 minutes is NOT suspected; it rejoins at once. -------------
    {
        World w;
        MobileMember phone(w.sim.s(kPhone), nullptr);
        check(w.sim.all_views_full(4), "setup: 4 members everywhere");
        const std::uint64_t inc_before = w.sim.s(kPhone).self_incarnation();

        phone.suspend(SuspendReason::AppBackground, 12 * kMinute);
        w.deliver_now();
        for (std::uint64_t p = 1; p <= 3; ++p) {
            check(w.sim.s(p).is_suspended(NodeId{kPhone}), "peer holds the phone's suspension lease");
            check(w.sim.s(p).health(NodeId{kPhone}).value().suspended, "health() reports suspended");
            check(w.sim.s(p).status_of(NodeId{kPhone}) == MemberStatus::Alive, "suspended is not Suspect");
            check(w.ev[p].count(MembershipEventKind::Suspended, kPhone) == 1, "exactly one Suspended event");
        }

        w.run_for(10 * kMinute);  // 5000 protocol periods; the phone does not tick (suspended)
        std::uint64_t suppressed = 0;
        for (std::uint64_t p = 1; p <= 3; ++p) {
            check(w.sim.s(p).status_of(NodeId{kPhone}) == MemberStatus::Alive, "C1: phone still Alive after 10 min");
            check(w.sim.s(p).view().contains(NodeId{kPhone}), "C1: phone still in the view after 10 min");
            check(w.ev[p].count(MembershipEventKind::Suspect, kPhone) == 0, "C1: no Suspect event for the phone");
            check(w.ev[p].count(MembershipEventKind::Dead, kPhone) == 0, "C1: no Dead event for the phone");
            check(w.ev[p].count(MembershipEventKind::Left, kPhone) == 0, "C1: no Left event for the phone");
            check(w.sim.s(p).is_suspended(NodeId{kPhone}), "lease still held at 10 min (< 12 min lease)");
            suppressed += w.sim.s(p).suspicions_suppressed();
        }
        std::printf("  C1: 10 min suspended: peers' Suspect/Dead events for the phone = 0, suppressed accusations = %llu\n",
                    static_cast<unsigned long long>(suppressed));
        check(w.sim.s(kPhone).self_suspended(), "phone is still suspended");

        // Resume. Bound asserted: the rejoin is complete after ONE fabric flush, ZERO protocol ticks.
        const std::int64_t ticks_before = w.sim.tick_no;
        phone.resume();
        w.deliver_now();
        check(w.sim.tick_no == ticks_before, "C3: no protocol tick was needed to rejoin");
        check(w.sim.s(kPhone).self_incarnation() > inc_before, "C3: incarnation raised on resume");
        for (std::uint64_t p = 1; p <= 3; ++p) {
            check(!w.sim.s(p).is_suspended(NodeId{kPhone}), "C3: peer cleared the lease on the first frame");
            check(w.sim.s(p).status_of(NodeId{kPhone}) == MemberStatus::Alive, "C3: phone Alive at peers");
            check(w.ev[p].count(MembershipEventKind::Resumed, kPhone) == 1, "C3: exactly one Resumed event");
        }
        // No false suspicion raised BY the phone after its 10-minute sleep, and views stay full: no churn.
        w.run_for(5'000 * MS);
        check(w.sim.all_views_full(4), "C3: views full 5 s after resume");
        for (std::uint64_t i = 1; i <= 4; ++i) {
            check(w.ev[i].count_any(MembershipEventKind::Suspect) == 0, "C3: no Suspect event anywhere, ever");
            check(w.ev[i].count_any(MembershipEventKind::Dead) == 0, "C3: no Dead event anywhere, ever");
        }
    }

    // ---- C1 CONTROL: the same 10-minute absence WITHOUT suspend() is suspected, then Dead. ------------
    {
        World w;
        w.sim.s(kPhone).set_online(false);  // process frozen but never announced it
        std::int64_t first_suspect = -1;
        const std::int64_t t0 = w.sim.now();
        const std::int64_t end = t0 + 10 * kMinute;
        while (w.sim.now() < end) {
            w.sim.round();
            if (first_suspect < 0 && w.ev[1].count(MembershipEventKind::Suspect, kPhone) > 0)
                first_suspect = (w.sim.now() - t0) / MS;
        }
        std::printf("  C1 control: silent without suspend(): first Suspect at +%lld ms\n",
                    static_cast<long long>(first_suspect));
        check(first_suspect >= 0 && first_suspect < 5'000, "control: peers suspect a silent member within seconds");
        for (std::uint64_t p = 1; p <= 3; ++p) {
            check(w.sim.s(p).status_of(NodeId{kPhone}) == MemberStatus::Dead, "control: peer declared the phone Dead");
            check(!w.sim.s(p).view().contains(NodeId{kPhone}), "control: phone left the view");
            check(w.ev[p].count(MembershipEventKind::Dead, kPhone) == 1, "control: a Dead event fired");
        }
    }

    // ---- C4: a peer that MISSED the announcement raises suspicion, but the member heals on resume. ----
    {
        World w;
        MobileMember phone(w.sim.s(kPhone), nullptr);
        w.sim.fab.link = [](NodeId a, NodeId b) {  // everything from 4 to 3 is lost (incl. the Suspend)
            return (a.value == kPhone && b.value == 3) ? Link::Drop : Link::Deliver;
        };
        phone.suspend(SuspendReason::Doze, 12 * kMinute);
        w.deliver_now();
        check(w.sim.s(1).is_suspended(NodeId{kPhone}) && w.sim.s(2).is_suspended(NodeId{kPhone}),
              "peers 1,2 heard the announcement");
        check(!w.sim.s(3).is_suspended(NodeId{kPhone}), "peer 3 did not");
        w.run_for(2 * kMinute);
        check(w.sim.s(3).status_of(NodeId{kPhone}) == MemberStatus::Dead, "peer 3 (deaf) declared it Dead");
        check(w.sim.s(1).status_of(NodeId{kPhone}) == MemberStatus::Alive &&
                  w.sim.s(2).status_of(NodeId{kPhone}) == MemberStatus::Alive,
              "C1: peers holding the lease were NOT dragged along by peer 3's gossip");
        check(w.sim.s(1).suspicions_suppressed() + w.sim.s(2).suspicions_suppressed() > 0,
              "the second-hand accusations were counted as suppressed");
        w.sim.fab.link = nullptr;  // the radio is back
        phone.resume();
        w.deliver_now();
        for (int t = 0; t < 40; ++t) w.sim.round();
        check(w.sim.s(3).status_of(NodeId{kPhone}) == MemberStatus::Alive, "C4: deaf peer's Dead was refuted by resume");
        check(w.sim.all_views_full(4), "C4: every view full again within 40 periods of resume");
    }

    // ---- C5: a STALE suspend (incarnation below what the receiver holds), a zero lease, and one from a
    //          non-member are ignored. ------------------------------------------------------------------
    {
        World w;
        auto inject = [&](std::uint64_t from, std::uint64_t to, std::uint64_t incarnation, std::uint64_t lease) {
            ControlMsg m;
            m.kind = ControlKind::Suspend;
            m.cluster = kCluster;
            m.from = NodeId{from};
            m.from_incarnation = incarnation;
            m.seq = lease;
            m.wire_version = kSwimWireVersion;
            MessageFrame f;
            f.from = NodeId{from};
            f.to = NodeId{to};
            f.kind = FrameKind::Control;
            f.payload = detail::encode_control(m);
            w.sim.fab.send(NodeId{from}, NodeId{to}, std::move(f));
            w.deliver_now();
        };
        const std::uint64_t inc = w.sim.s(1).incarnation_of(NodeId{kPhone});
        // Raise the phone's incarnation first so a replay of an older announcement is observable.
        w.sim.s(kPhone).suspend(SuspendReason::Doze, 5 * kMinute);
        w.sim.s(kPhone).resume();
        w.deliver_now();
        w.sim.round();
        const std::uint64_t cur = w.sim.s(1).incarnation_of(NodeId{kPhone});
        check(cur > inc, "setup: incarnation advanced by a suspend/resume cycle");
        check(!w.sim.s(1).is_suspended(NodeId{kPhone}), "setup: lease cleared by resume");
        inject(kPhone, 1, cur - 1, static_cast<std::uint64_t>(10 * kMinute));  // replay of an OLD suspend
        check(!w.sim.s(1).is_suspended(NodeId{kPhone}), "C5: stale-incarnation suspend ignored");
        inject(kPhone, 1, cur, 0);  // zero lease
        check(!w.sim.s(1).is_suspended(NodeId{kPhone}), "C5: zero lease ignored");
        inject(99, 1, 1, static_cast<std::uint64_t>(10 * kMinute));  // unknown sender
        check(!w.sim.s(1).is_suspended(NodeId{99}), "C5: suspend from a non-member ignored");
        // CONTROL: the same frame at the current incarnation IS accepted.
        inject(kPhone, 1, cur, static_cast<std::uint64_t>(10 * kMinute));
        check(w.sim.s(1).is_suspended(NodeId{kPhone}), "C5 control: a current-incarnation suspend is accepted");
    }

    std::printf(g_ok ? "mobile_member_suspend_test: OK\n" : "mobile_member_suspend_test: FAILED\n");
    return g_ok ? 0 : 1;
}
