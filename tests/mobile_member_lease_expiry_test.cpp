// Tests ADR-071 claims C2 (a lapsed lease ends in the ordinary removal path: no zombie member) and C6
// (the RECEIVER bounds both the lease length and the number of leases it holds), per 021 §"Mobile member
// profile". Virtual clock, deferred-delivery fabric.
//
// Controls: a lease still inside its bound keeps the member Alive (the removal is not simply early);
// a lease request beyond the cap is clamped (the member is not parked forever); a lease past the table cap
// is refused and that member is suspected like any silent one.
#include <cstdint>
#include <cstdio>

#include "quark/core/mobile_member.hpp"
#include "swim_deferred_fabric.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr ClusterId kCluster{0x1EA5E};
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
    c.seed = 0xBEEF;
    c.tombstone_ttl_ns = 120 * kMinute;
    c.mobile.max_lease_ns = 15 * kMinute;
    return c;
}

void settle(Sim& sim) {
    for (int t = 0; t < 30; ++t) sim.round();
}
void run_until(Sim& sim, std::int64_t t_ns) {
    while (sim.now() < t_ns) sim.round();
}
void deliver(Sim& sim) { sim.fab.flush(16, 0, 0, INT64_MAX, sim.clk); }
}  // namespace

int main() {
    // ---- C2: lease expires, member never resumes -> Suspect -> Dead, removed from every view. ---------
    {
        Sim sim;
        sim.cfg = base_cfg();
        for (std::uint64_t i = 1; i <= 3; ++i) sim.add(i);
        sim.admit_all();
        settle(sim);
        const std::int64_t t0 = sim.now();
        sim.s(3).suspend(SuspendReason::AppBackground, 10 * kMinute);
        deliver(sim);
        check(sim.s(1).is_suspended(NodeId{3}) && sim.s(2).is_suspended(NodeId{3}), "leases granted");

        run_until(sim, t0 + 10 * kMinute - 2'000 * MS);  // control: just inside the bound
        check(sim.s(1).status_of(NodeId{3}) == MemberStatus::Alive, "control: Alive 2 s before the lease ends");
        check(sim.s(1).view().contains(NodeId{3}), "control: still in the view 2 s before the lease ends");

        std::int64_t dead_at = -1, suspect_at = -1;
        const std::int64_t end = t0 + 12 * kMinute;
        while (sim.now() < end) {
            sim.round();
            if (suspect_at < 0 && sim.s(1).status_of(NodeId{3}) == MemberStatus::Suspect)
                suspect_at = (sim.now() - t0) / MS;
            if (dead_at < 0 && sim.s(1).status_of(NodeId{3}) == MemberStatus::Dead) dead_at = (sim.now() - t0) / MS;
        }
        std::printf("  C2: lease 10 min: Suspect at +%lld ms, Dead at +%lld ms (suspicion timeout 500 ms)\n",
                    static_cast<long long>(suspect_at), static_cast<long long>(dead_at));
        check(suspect_at >= 10 * 60'000 - 200 && suspect_at < 10 * 60'000 + 1'000, "Suspect right after the lease ends");
        check(dead_at >= suspect_at + 500 && dead_at < suspect_at + 2'000, "Dead one suspicion window later");
        for (std::uint64_t p = 1; p <= 2; ++p) {
            check(!sim.s(p).view().contains(NodeId{3}), "C2: the member is gone from the view (no zombie)");
            check(sim.s(p).suspended_count() == 0, "C2: no lease left behind");
        }
    }

    // ---- C2b: a LAPSED lease cannot be renewed at the same incarnation (a member that never answers cannot
    //           keep itself alive by re-announcing). Control: a lease inside its bound CAN be refreshed. ------
    {
        Sim sim;
        sim.cfg = base_cfg();
        for (std::uint64_t i = 1; i <= 3; ++i) sim.add(i);
        sim.admit_all();
        settle(sim);
        auto inject_suspend = [&](std::uint64_t incarnation) {
            ControlMsg m;
            m.kind = ControlKind::Suspend;
            m.cluster = kCluster;
            m.from = NodeId{3};
            m.from_incarnation = incarnation;
            m.seq = static_cast<std::uint64_t>(10 * kMinute);
            m.wire_version = kSwimWireVersion;
            MessageFrame f;
            f.from = NodeId{3};
            f.to = NodeId{1};
            f.kind = FrameKind::Control;
            f.payload = detail::encode_control(m);
            sim.fab.send(NodeId{3}, NodeId{1}, std::move(f));
            deliver(sim);
        };
        const std::int64_t t0 = sim.now();
        const std::uint64_t inc = sim.s(1).incarnation_of(NodeId{3});
        sim.s(3).suspend(SuspendReason::AppBackground, 1 * kMinute);
        deliver(sim);
        run_until(sim, t0 + 30'000 * MS);
        inject_suspend(inc);  // inside the bound: a refresh at the same incarnation is accepted
        check(sim.s(1).is_suspended(NodeId{3}), "C2b control: a refresh inside the lease is accepted");
        const std::int64_t t1 = sim.now();
        run_until(sim, t1 + 10 * kMinute + 200 * MS);  // the (refreshed, 10 min) lease lapses
        check(sim.s(1).status_of(NodeId{3}) == MemberStatus::Suspect, "C2b: lapsed lease -> Suspect");
        inject_suspend(inc);
        check(!sim.s(1).is_suspended(NodeId{3}), "C2b: re-announcing at the same incarnation after expiry is refused");
        check(sim.s(1).status_of(NodeId{3}) == MemberStatus::Suspect, "C2b: still Suspect, heading for Dead");
    }

    // ---- C6a: the receiver clamps an absurd requested lease (~100 days asked, 15 min cap). -------------
    {
        Sim sim;
        sim.cfg = base_cfg();
        for (std::uint64_t i = 1; i <= 3; ++i) sim.add(i);
        sim.admit_all();
        settle(sim);
        const std::int64_t t0 = sim.now();
        sim.s(3).suspend(SuspendReason::Other, 100LL * 24 * 60 * kMinute);  // 100 days
        deliver(sim);
        run_until(sim, t0 + 15 * kMinute - 2'000 * MS);
        check(sim.s(1).status_of(NodeId{3}) == MemberStatus::Alive, "C6a: still Alive inside the cap");
        run_until(sim, t0 + 15 * kMinute + 5'000 * MS);
        check(sim.s(1).status_of(NodeId{3}) == MemberStatus::Dead, "C6a: Dead shortly after the 15 min cap");
        check(!sim.s(1).view().contains(NodeId{3}), "C6a: removed from the view");
    }

    // ---- C6b: the lease TABLE is bounded; the member past the cap is treated as silent. -----------------
    {
        Sim sim;
        sim.cfg = base_cfg();
        sim.cfg.mobile.max_suspended = 2;
        for (std::uint64_t i = 1; i <= 4; ++i) sim.add(i);
        sim.admit_all();
        settle(sim);
        for (std::uint64_t p = 2; p <= 4; ++p) sim.s(p).suspend(SuspendReason::Doze, 10 * kMinute);
        deliver(sim);
        check(sim.s(1).suspended_count() == 2, "C6b: node 1 holds exactly max_suspended=2 leases");
        const std::int64_t t0 = sim.now();
        run_until(sim, t0 + 30'000 * MS);
        int dead = 0, alive = 0;
        for (std::uint64_t p = 2; p <= 4; ++p) {
            if (sim.s(1).status_of(NodeId{p}) == MemberStatus::Dead) ++dead;
            if (sim.s(1).status_of(NodeId{p}) == MemberStatus::Alive) ++alive;
        }
        check(alive == 2 && dead == 1, "C6b: 2 leased members stay Alive, the one past the cap is removed");
    }

    std::printf(g_ok ? "mobile_member_lease_expiry_test: OK\n" : "mobile_member_lease_expiry_test: FAILED\n");
    return g_ok ? 0 : 1;
}
