// Tests ADR-048 relay side, shape B (issue #14): a node with set_relay_forwarding() forwards transit
// frames (to != self) byte-for-byte; the admit predicate gates it (fail-closed open-proxy guard); the
// origin must match the connection's identity; set_relay_via() gives an ordered failover relay list.
// Topology: spoke A and spoke B each know ONLY relay R (hub-and-spoke); A -> B must cross R.
#include <cstdint>
#include <cstdio>

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
    const NodeId A{1}, R{2}, B{3}, DEAD{9};

    net::TcpTransport tr(R, pal::ipv4_loopback, 0);
    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    Recorder rb, rr;
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    tr.on_receive([&](MessageFrame f) { rr(std::move(f)); });
    std::atomic<bool> allow{false};
    tr.set_relay_forwarding([&](NodeId, NodeId) { return allow.load(); });
    check(tr.start() && ta.start() && tb.start(), "all three listeners start", ok);
    if (!ok) return 1;

    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));
    // Failover list: the first entry is a relay that does not exist, so routing must pick R.
    ta.set_relay_via(B, {DEAD, R});
    ta.add_peer(loopback_endpoint(DEAD, 1));
    // Make R's connection Open first so route_for() prefers it over the dead entry.
    ta.send(R, seq_frame(A, R, 100));
    check(wait_until([&] { return rr.count() >= 1; }, 3000), "A reaches R directly", ok);

    // 1. Fail-closed: predicate says no ⇒ nothing is forwarded, nothing is delivered to R locally.
    ta.send(B, seq_frame(A, B, 0));
    check(wait_until([&] { return tr.relay_refused() >= 1; }, 3000), "refused when admit() says no", ok);
    check(rb.count() == 0 && rr.count() == 1, "refused frame neither forwarded nor delivered at R", ok);

    // 2. Admitted peers are relayed, in order, byte-for-byte.
    allow = true;
    for (std::uint64_t i = 1; i <= 20; ++i) ta.send(B, seq_frame(A, B, i));
    check(wait_until([&] { return rb.count() >= 20; }, 5000), "20 frames relayed A -> R -> B", ok);
    {
        std::lock_guard<std::mutex> g(rb.m);
        for (std::size_t i = 0; i < rb.seqs.size(); ++i)
            check(rb.seqs[i] == i + 1, "relayed frames keep FIFO order and content", ok);
    }
    check(tr.relayed_frames() == 20, "R counted 20 relayed frames", ok);
    check(rr.count() == 1, "transit frames are never delivered to the relay's own sink", ok);

    // 3. Identity: a frame whose `from` is not the connection's peer is refused (no laundering).
    const auto refused_before = tr.relay_refused();
    ta.send(B, seq_frame(NodeId{77}, B, 500));
    check(wait_until([&] { return tr.relay_refused() > refused_before; }, 3000),
          "spoofed `from` refused", ok);
    check(rb.count() == 20, "spoofed frame not delivered", ok);

    // 4. Unknown destination is refused, never dialed blind.
    const auto refused2 = tr.relay_refused();
    ta.set_relay_via(NodeId{50}, {R});
    ta.send(NodeId{50}, seq_frame(A, NodeId{50}, 600));
    check(wait_until([&] { return tr.relay_refused() > refused2; }, 3000),
          "unknown destination refused", ok);

    ta.stop(); tb.stop(); tr.stop();
    std::printf(ok ? "tcp_relay_forwarding_test: PASS\n" : "tcp_relay_forwarding_test: FAIL\n");
    return ok ? 0 : 1;
}
