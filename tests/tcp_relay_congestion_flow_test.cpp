// Tests issue #14 — the FULL ADR-046 Congested flow across a relay hop. Two real SwimMembership nodes
// (A, B) over real TcpTransports that know only a forwarding relay R: B's mailbox pressure crosses the
// watermark, B's SwimMembership broadcasts `Congested`, the frame crosses R (Control lane, forwarded
// byte-for-byte, m.from == authenticated frame.from == B so SWIM's impersonation check passes), A's
// SwimMembership marks B congested on A's OWN TcpTransport, and A's admit_send(B) sheds. Then the TTL
// self-heals admission with no second frame.
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <thread>

#include "quark/core/cluster.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
void check(bool c, const char* what, bool& ok) {
    if (!c) {
        std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{1414};
    const NodeId A{1}, R{2}, B{3};

    net::TcpTransport tr(R, pal::ipv4_loopback, 0), ta(A, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
    tr.on_receive([](MessageFrame) {});
    tr.set_relay_forwarding([&](NodeId, NodeId) { return true; });

    SwimMembership::Config cfg;
    cfg.cluster_id = cluster;
    cfg.seed = 0xC0FFEE;
    cfg.congestion_high_watermark = 100;
    cfg.congestion_low_watermark = 50;
    cfg.congestion_window_ns = 800'000'000;   // 0.8s TTL
    cfg.congestion_refresh_ns = 5'000'000'000;  // no re-broadcast inside the test window
    SwimMembership swimA(A, ta, cfg), swimB(B, tb, cfg);
    (void)swimA.admit(B, cluster);
    (void)swimB.admit(A, cluster);
    std::atomic<std::uint64_t> b_resident{200};  // above the high watermark
    swimB.set_pressure_gossip([&] { return b_resident.load(); }, [](const std::vector<MailboxPressureEntry>&) {});

    check(tr.start() && ta.start() && tb.start(), "listeners start", ok);
    if (!ok) return 1;
    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(A, ta.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));
    ta.set_relay_via(B, {R});
    tb.set_relay_via(A, {R});

    check(ta.admit_send(B), "before any signal A admits sends to B", ok);
    bool throttled = wait_until(
        [&] {
            // Drive B's SWIM on its own transport's I/O thread (the same thread that delivers B's
            // inbound frames), so tick() and on_frame() never overlap. The tick fires the edge-triggered
            // Congested broadcast (B past the watermark). post() is async: sleep so ticks are not flooded.
            tb.post([&] { swimB.tick(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            for (int i = 0; i < 200; ++i)
                if (!ta.admit_send(B)) return true;  // the bucket drained ⇒ A's gate is Shedding
            return false;
        },
        8000);
    check(throttled, "B's Congested crossed the relay and throttled A's own transport", ok);
    check(tr.relayed_frames() >= 1, "the signal was carried by the relay (not a direct link)", ok);

    auto sheds_in_burst = [&] {
        int shed = 0;
        for (int i = 0; i < 500; ++i)
            if (!ta.admit_send(B)) ++shed;
        return shed;
    };
    check(sheds_in_burst() > 0, "while inside the congestion window a burst is partly shed", ok);

    b_resident = 0;  // pressure gone; no further Congested frames
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.congestion_window_ns / 1'000'000 + 200));
    check(sheds_in_burst() == 0, "after the TTL a burst is fully admitted again (no explicit clear frame)", ok);
    ta.stop(); tb.stop(); tr.stop();
    std::printf("tcp_relay_congestion_flow_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
