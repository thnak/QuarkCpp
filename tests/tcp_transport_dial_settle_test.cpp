// #65 fallback controls for the DialHint settle handshake:
//  1. a v1 peer speaks no hint capability -> the v2 dialer must confirm immediately (old behavior), never
//     wait for a hint that will never come;
//  2. a v2 peer that suppresses its hint -> the dialer must still confirm on the settle timeout and deliver
//     (liveness: a silent/misbehaving peer can never wedge a connection forever).
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
    constexpr std::uint64_t M = 100;

    // ---- 1) v1 peer: no hint capability either way -------------------------------------------------
    {
        net::TcpTransport ta(A, pal::ipv4_loopback, 0);
        net::TcpTransport tb(B, pal::ipv4_loopback, 0);
        tb.set_hello_version(1);  // B advertises v1: no DialHint
        Recorder rb;
        tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
        check(ta.start() && tb.start(), "v1 case: both listeners start", ok);
        ta.add_peer(loopback_endpoint(B, tb.listen_port()));
        tb.add_peer(loopback_endpoint(A, ta.listen_port()));
        for (std::uint64_t i = 0; i < M; ++i) ta.send(B, seq_frame(A, B, i));
        check(wait_until([&] { return rb.count() == M; }), "v1 case: all frames delivered", ok);
        check(ta.settle_deferred() == 0, "v1 case: the v2 dialer does not wait for a hint", ok);
        check(tb.dial_hints_sent() == 0, "v1 case: the v1 acceptor sends no hint", ok);
        ta.stop();
        tb.stop();
    }

    // ---- 2) suppressed hint: the settle timeout is the liveness fallback ---------------------------
    {
        net::TcpTransport ta(A, pal::ipv4_loopback, 0);
        net::TcpTransport tb(B, pal::ipv4_loopback, 0);
        net::TcpTestFaults faults;
        faults.suppress_dial_hint = true;
        tb.set_test_faults(faults);
        ta.set_dial_settle_timeout_ns(50'000'000);  // 50 ms
        Recorder rb;
        tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
        check(ta.start() && tb.start(), "timeout case: both listeners start", ok);
        ta.add_peer(loopback_endpoint(B, tb.listen_port()));
        tb.add_peer(loopback_endpoint(A, ta.listen_port()));
        for (std::uint64_t i = 0; i < M; ++i) ta.send(B, seq_frame(A, B, i));
        check(wait_until([&] { return rb.count() == M; }), "timeout case: all frames delivered", ok);
        check(ta.settle_timeouts() >= 1, "timeout case: the dialer confirmed on the settle timeout", ok);
        check(ta.dial_hints_received() == 0, "timeout case: no hint was received", ok);
        ta.stop();
        tb.stop();
    }

    std::printf("tcp_transport_dial_settle_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
