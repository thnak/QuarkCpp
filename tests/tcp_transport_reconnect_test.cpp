// Tests 021 §"Reconnect with jittered exponential backoff" over real sockets: after a peer drops the
// connection, the transport re-establishes it (jittered backoff) and resumes delivery, without the
// application re-driving anything. The drop is induced by the peer calling close_peer (its SWIM-death
// teardown hook); the surviving side sees EOF and reconnects because it knows the peer's endpoint.
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
    const NodeId A{1}, B{2};

    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    Recorder ra, rb;
    ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    check(ta.start() && tb.start(), "both listeners start", ok);
    if (!ok) {
        std::printf("tcp_transport_reconnect_test: FAIL (startup)\n");
        return 1;
    }
    // A knows B's endpoint (so A can re-dial). B does NOT re-dial after it tears down (close_peer
    // suppresses B's reconnect) — it just accepts A's fresh connection.
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));

    // Establish + deliver one frame.
    ta.send(B, seq_frame(A, B, 0));
    check(wait_until([&] { return rb.count() >= 1; }), "initial frame delivered A→B", ok);

    // B drops the connection to A (its SWIM-death teardown). A will see EOF.
    tb.close_peer(A);

    // A re-establishes on its own (jittered backoff ~20–40 ms first attempt).
    check(wait_until([&] { return ta.reconnects() >= 1; }, 4000), "A reconnected to B after the drop", ok);

    // Delivery resumes over the reconnected socket.
    const std::uint64_t before = rb.count();
    ta.send(B, seq_frame(A, B, 1));
    check(wait_until([&] { return rb.count() >= before + 1; }, 4000), "delivery resumes after reconnect",
          ok);
    check(wait_until([&] { return ta.connections_open() == 1; }), "A again holds one connection to B",
          ok);

    std::printf("tcp_transport_reconnect_test: %s  (reconnects=%llu, B recv=%llu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(ta.reconnects()),
                static_cast<unsigned long long>(rb.count()));

    ta.stop();
    tb.stop();
    return ok ? 0 : 1;
}
