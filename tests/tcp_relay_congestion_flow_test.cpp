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
#include <functional>
#include <mutex>
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

// SwimMembership is single-driver by contract (cluster.hpp): its inbound frames and tick() must not
// overlap. A real TCP transport delivers frames on its I/O thread, so this test serialises them with
// tick() through one mutex (TSan caught the unserialised version: on_frame vs tick on node B).
struct SerialisedTcp : Transport {  // TcpTransport is final: forward to it
    explicit SerialisedTcp(net::TcpTransport& inner) : inner_(inner) {}
    std::mutex drive_mu;
    void send(NodeId to, MessageFrame f) override { inner_.send(to, std::move(f)); }
    void on_receive(std::function<void(MessageFrame)> cb) override {
        inner_.on_receive([this, cb = std::move(cb)](MessageFrame f) {
            std::lock_guard<std::mutex> g(drive_mu);
            cb(std::move(f));
        });
    }
    [[nodiscard]] bool admit_send(NodeId peer) noexcept override { return inner_.admit_send(peer); }
    void mark_congested(NodeId peer, std::int64_t ns) noexcept override { inner_.mark_congested(peer, ns); }

private:
    net::TcpTransport& inner_;
};
}  // namespace

int main() {
    bool ok = true;
    const ClusterId cluster{1414};
    const NodeId A{1}, R{2}, B{3};

    net::TcpTransport tr(R, pal::ipv4_loopback, 0), ta(A, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
    SerialisedTcp tb_serial(tb);
    tr.on_receive([](MessageFrame) {});
    tr.set_relay_forwarding([&](NodeId, NodeId) { return true; });

    SwimMembership::Config cfg;
    cfg.cluster_id = cluster;
    cfg.seed = 0xC0FFEE;
    cfg.congestion_high_watermark = 100;
    cfg.congestion_low_watermark = 50;
    cfg.congestion_window_ns = 800'000'000;   // 0.8s TTL
    cfg.congestion_refresh_ns = 5'000'000'000;  // no re-broadcast inside the test window
    SwimMembership swimA(A, ta, cfg), swimB(B, tb_serial, cfg);
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
            {
                std::lock_guard<std::mutex> g(tb_serial.drive_mu);
                swimB.tick();
            }  // fires the edge-triggered Congested broadcast (B past the watermark)
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
