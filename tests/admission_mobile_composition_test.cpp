// Tests ADR-069 claim C16 / ADR-071 claim C9 (red-team #47, "untested by the shipped tests"): one node
// running SwimMembership + SecureTransport + TcpTransport that SHARE ONE AdmissionAuthority, holding a
// mobile suspension lease for a peer, when the application publishes a deny for that peer mid-run.
// Implements 020 §1a, 021 §"Application-driven eviction" and §"Mobile member profile" as amended by
// ADR-069/ADR-071 "Red-team of the merged changes (#47)". Real sockets, real clock, the mock handshake.
//
// Node A (id 1) is the node under test; node B (id 2) is the phone (SwimMembership + MobileMember over its own
// SecureTransport + TcpTransport, admitting A). Each node's SWIM runs on its transport's I/O thread (ticks are
// posted), as the AeroCoWorker design does.
//   P1 B joins A and both are Alive.
//   P2 B suspend()s (10 min lease): A holds the lease, one Suspended event, no Suspect/Dead.
//   P3 A's application denies B: within a bounded time A raises exactly ONE Evicted event, B leaves A's view,
//      A holds no lease for it, A's session for B is closed, A's TCP connection to B is closed and STAYS
//      closed (no redial, B's dials are refused at HELLO), and B resuming raises NO Resumed event at A.
//   P4 A's application admits B again: B is back in A's view (Joined) and traffic flows.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "admission_test_util.hpp"
#include "quark/core/mobile_member.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
bool g_ok = true;
void check(bool c, const char* what) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        g_ok = false;
    }
}

struct Counts {
    std::atomic<int> joined{0}, suspect{0}, dead{0}, evicted{0}, suspended{0}, resumed{0};
    void operator()(const MembershipEvent& e) {
        switch (e.kind) {
            case MembershipEventKind::Joined: ++joined; break;
            case MembershipEventKind::Suspect: ++suspect; break;
            case MembershipEventKind::Dead: ++dead; break;
            case MembershipEventKind::Evicted: ++evicted; break;
            case MembershipEventKind::Suspended: ++suspended; break;
            case MembershipEventKind::Resumed: ++resumed; break;
            default: break;
        }
    }
};

// Run `fn` on `t`'s I/O thread (where SWIM state lives) and return its result.
template <class Fn>
auto on_loop(net::TcpTransport& t, Fn fn) {
    std::promise<decltype(fn())> p;
    auto f = p.get_future();
    t.post([&] { p.set_value(fn()); });
    return f.get();
}
// control = the pre-fix wiring: no transport admission (reset close hook) and leases kept on tombstone.
void run_once(bool control) {
    const NodeId A{1}, B{2};
    const ClusterId cluster{0xC0DE};
    AdmissionAuthority authA, authB;  // declared first: they outlive every listener owner
    net::TcpTransport tA(A, pal::ipv4_loopback, 0), tB(B, pal::ipv4_loopback, 0);
    MockCipher unused{0};
    SecureTransport sA{tA, unused, A}, sB{tB, unused, B};
    auto id_blob = std::make_shared<const TlsIdentity>(TlsIdentity{});
    auto trust_blob = std::make_shared<const TrustedRoots>(TrustedRoots{});
    IdentityMaterial iA{id_blob}, iB{id_blob};
    TrustStore rA{trust_blob}, rB{trust_blob};
    MockHandshakeFactory fA, fB;
    sA.enable_handshake(fA, ClusterId{555}, iA, rA);
    sB.enable_handshake(fB, ClusterId{555}, iB, rB);
    if (!control) {
        tA.set_admission(authA);  // the transport, the session layer and SWIM share ONE authority on node A
        sA.set_admission(authA, {});
    } else {
        sA.set_admission(authA, [&](NodeId p) { tA.reset_peer_connection(p); });
    }
    tB.set_admission(authB);
    sB.set_admission(authB, {});
    tA.set_peer_down_hook([&](NodeId p) { sA.on_peer_disconnected(p); });
    tB.set_peer_down_hook([&](NodeId p) { sB.on_peer_disconnected(p); });

    SwimMembership::Config cfg;
    cfg.cluster_id = cluster;
    cfg.ack_timeout_ns = 100'000'000;
    cfg.suspicion_timeout_ns = 600'000'000;
    cfg.authority_only_revocation = true;
    cfg.test_faults.keep_lease_on_tombstone = control;
    SwimMembership swimA(A, sA, cfg), swimB(B, sB, cfg);
    swimA.set_admission(authA);
    MobileMember mobileB(swimB, [&](NodeId p) { tB.reset_peer_connection(p); });
    Counts evA;
    swimA.add_event_sink([&](const MembershipEvent& e) { evA(e); });
    (void)authA.publish(snap(1, {B}));
    (void)authB.publish(snap(1, {A}));
    if (!(tA.start() && tB.start())) {
        check(false, "transports start");
        return;
    }
    tA.add_peer(loopback_endpoint(B, tB.listen_port()));
    tB.add_peer(loopback_endpoint(A, tA.listen_port()));
    (void)swimA.admit(B, cluster);
    (void)swimB.admit(A, cluster);

    std::atomic<bool> run{true};
    std::thread ticker([&] {
        while (run.load()) {
            tA.post([&] { swimA.tick(); });
            tB.post([&] { swimB.tick(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    auto a_has_b = [&] { return on_loop(tA, [&] { return swimA.view().contains(B); }); };

    // P1
    tB.post([&] { swimB.request_join(A); });
    check(wait_until([&] { return a_has_b() && on_loop(tA, [&] { return swimA.status_of(B) == MemberStatus::Alive; }); }, 10000),
          "P1: B is in A's view and Alive");

    check(wait_until([&] { return on_loop(tB, [&] { return swimB.status_of(A) == MemberStatus::Alive && swimB.view().contains(A); }); }, 10000),
          "P1: A is in B's view and Alive");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // P2 (a Suspect during start-up, while the first handshakes complete, is not what is being tested)
    const int suspect0 = evA.suspect.load(), dead0 = evA.dead.load();
    tB.post([&] { mobileB.suspend(SuspendReason::AppBackground, 10LL * 60 * 1'000'000'000); });
    check(wait_until([&] { return on_loop(tA, [&] { return swimA.is_suspended(B); }); }, 5000), "P2: A holds B's lease");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // long past suspicion + ack timeouts
    std::printf("P2: Suspended=%d Suspect=%d Dead=%d lease=%d\n", evA.suspended.load(), evA.suspect.load(), evA.dead.load(),
                (int)on_loop(tA, [&] { return swimA.is_suspended(B); }));
    check(evA.suspended.load() == 1 && evA.suspect.load() == suspect0 && evA.dead.load() == dead0,
          "P2: one Suspended event, no new Suspect/Dead while leased");

    // P3
    const std::uint64_t closed0 = sA.admission_stats().sessions_closed;
    (void)authA.publish(snap(2, {}));  // A's application denies B (absent == Deny)
    const bool evicted = wait_until([&] { return evA.evicted.load() == 1; }, 5000);
    check(evicted, "P3: exactly one Evicted event at A");
    tB.post([&] { mobileB.resume(); });  // the phone wakes, not told, and talks / redials
    const std::uint64_t r0 = tA.reconnects();
    std::uint64_t open_samples = 0, samples = 0;
    for (int ms = 0; ms < 3000; ms += 20) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ++samples;
        if (ms >= 400 && tA.connections_open() > 0) ++open_samples;
    }
    const bool in_view = a_has_b();
    const bool lease = on_loop(tA, [&] { return swimA.is_suspended(B); });
    std::printf("P3: Evicted=%d in_view=%d lease=%d Resumed=%d sessions_closed(+)=%llu session=%d conn_open_samples=%llu/%llu "
                "A.reconnects(+)=%llu A.refused=%llu\n",
                evA.evicted.load(), (int)in_view, (int)lease, evA.resumed.load(),
                (unsigned long long)(sA.admission_stats().sessions_closed - closed0),
                (int)sA.session_admission_generation(B).has_value(), (unsigned long long)open_samples,
                (unsigned long long)samples, (unsigned long long)(tA.reconnects() - r0),
                (unsigned long long)tA.admission_refused());
    if (!control) {
        check(evA.evicted.load() == 1, "P3: still exactly one Evicted");
        check(!in_view, "P3: B is out of A's view");
        check(!lease, "P3: A holds no lease for the evicted member");
        check(evA.resumed.load() == 0, "P3: no Resumed event while B is denied");
        check(!sA.session_admission_generation(B).has_value(), "P3: A's session for B is closed");
        check(open_samples == 0, "P3: A's TCP connection to B stays closed");
        check(tA.reconnects() == r0, "P3: A never redials B");
    } else {
        check(lease, "P3 control: the old behaviour leaves A holding a lease for the evicted member");
        check(open_samples > 0, "P3 control: the old wiring leaves A holding or reopening a connection to the denied device");
    }

    if (!control) {
        // P4
        (void)authA.publish(snap(3, {B}));
        const bool back = wait_until(
            [&] {
                tB.post([&] { swimB.request_join(A); });
                return a_has_b();
            },
            15000);
        std::printf("P4: re-admitted: in_view=%d Joined=%d\n", (int)back, evA.joined.load());
        check(back, "P4: a later admitting snapshot lets B back in");
        check(evA.joined.load() >= 2, "P4: a fresh Joined event");
    }

    run.store(false);
    ticker.join();
    tA.stop();
    tB.stop();
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    run_once(false);
    std::printf("CONTROL (pre-fix wiring)\n");
    run_once(true);
    std::printf(g_ok ? "admission_mobile_composition_test: PASS\n" : "admission_mobile_composition_test: FAIL\n");
    return g_ok ? 0 : 1;
}
