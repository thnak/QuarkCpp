// Tests 021 §2 dial deduplication REALIZED over real racing sockets (cluster_dial_dedup_test proves the
// pure rule; this proves the transport enforces it). When A and B dial each other simultaneously, two
// TCP connections form; the lower-NodeId-initiated one is kept and the other closed, so each node
// converges to EXACTLY ONE connection per peer (010's one-connection-per-peer invariant) with traffic
// intact across the collapse.
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
        std::printf("tcp_transport_dial_dedup_test: FAIL (startup)\n");
        return 1;
    }
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));

    // Force the concurrency hazard: both nodes send (⇒ dial) at the same instant, so two connections
    // race into existence. Repeated a few rounds to make the double-dial likely on a fast loopback.
    std::uint64_t seq = 0;
    for (int round = 0; round < 5; ++round) {
        ta.send(B, seq_frame(A, B, seq));
        tb.send(A, seq_frame(B, A, seq));
        ++seq;
    }

    // THE 010/021 INVARIANT: each node converges to EXACTLY ONE connection to its peer. This is the real
    // guarantee — whether or not a socket actually raced (dedup_closed>0 means a redundant one was
    // reaped). Frames from the sub-ms race window itself are at-most-once (may drop as the loser closes),
    // so this test does NOT assert the race burst was fully delivered — it asserts CONVERGENCE, then
    // proves LOSSLESS delivery over the settled connection below.
    check(wait_until([&] { return ta.connections_open() == 1; }), "A converges to ONE connection to B",
          ok);
    check(wait_until([&] { return tb.connections_open() == 1; }), "B converges to ONE connection to A",
          ok);
    // Let any reconnect/dedup churn from the race fully quiesce before measuring lossless delivery.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Post-settle: the surviving single connection carries a full batch both ways with NO loss.
    const std::uint64_t a_before = ra.count(), b_before = rb.count();
    constexpr std::uint64_t M = 200;
    for (std::uint64_t i = 0; i < M; ++i) {
        ta.send(B, seq_frame(A, B, 1000 + i));
        tb.send(A, seq_frame(B, A, 2000 + i));
    }
    check(wait_until([&] { return rb.count() >= b_before + M && ra.count() >= a_before + M; }),
          "the surviving connection carries a full batch both ways, losslessly, after dedup", ok);
    // And it is STILL one connection per peer after the batch (no new socket crept in).
    check(ta.connections_open() == 1 && tb.connections_open() == 1,
          "still exactly one connection per peer after post-settle traffic", ok);

    std::printf(
        "tcp_transport_dial_dedup_test: %s  (A dedup_closed=%llu, B dedup_closed=%llu; A open=%llu B "
        "open=%llu)\n",
        ok ? "OK" : "FAIL", static_cast<unsigned long long>(ta.dedup_closed()),
        static_cast<unsigned long long>(tb.dedup_closed()),
        static_cast<unsigned long long>(ta.connections_open()),
        static_cast<unsigned long long>(tb.connections_open()));

    ta.stop();
    tb.stop();
    return ok ? 0 : 1;
}
