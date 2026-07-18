// Tests the 010 default TcpTransport over real 127.0.0.1 sockets — the byte-moving contract the seam
// promises, end to end: two nodes on ephemeral ports send to each other, and every frame arrives, in
// order, with its payload intact. This is the socket-level proof of "one connection per peer, FIFO per
// (sender,receiver)" (010): a single sender's frames land at the receiver in send order because they
// were appended to one ordered write buffer and reassembled by one FrameStream.
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
    constexpr std::uint64_t N = 400;
    const NodeId A{1}, B{2};

    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    Recorder ra, rb;
    ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });

    check(ta.start(), "node A listener starts", ok);
    check(tb.start(), "node B listener starts", ok);
    if (!ok) {
        std::printf("tcp_transport_loopback_test: FAIL (startup)\n");
        return 1;
    }

    // Teach each side where the other listens (021 Discovery/gossip supplies this in a real cluster).
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));

    // ---- A → B: lazy dial on the first send, then N frames in order ----------------------------
    for (std::uint64_t i = 0; i < N; ++i) ta.send(B, seq_frame(A, B, i));
    check(wait_until([&] { return rb.count() >= N; }), "B received all N frames from A", ok);

    // ---- B → A over the (deduplicated) reverse direction ---------------------------------------
    for (std::uint64_t i = 0; i < N; ++i) tb.send(A, seq_frame(B, A, i));
    check(wait_until([&] { return ra.count() >= N; }), "A received all N frames from B", ok);

    // FIFO + payload integrity on both directions.
    {
        std::lock_guard<std::mutex> g(rb.m);
        bool ordered = rb.seqs.size() >= N;
        for (std::uint64_t i = 0; i < N && i < rb.seqs.size(); ++i) {
            if (rb.seqs[i] != i) ordered = false;
            if (rb.payloads[i].size() != 1 ||
                rb.payloads[i][0] != static_cast<std::byte>(i & 0xFF))
                ordered = false;
        }
        check(ordered, "A→B: per-(sender,receiver) FIFO + payload intact", ok);
    }
    {
        std::lock_guard<std::mutex> g(ra.m);
        bool ordered = ra.seqs.size() >= N;
        for (std::uint64_t i = 0; i < N && i < ra.seqs.size(); ++i)
            if (ra.seqs[i] != i) ordered = false;
        check(ordered, "B→A: per-(sender,receiver) FIFO preserved", ok);
    }

    // One connection per peer (010 invariant): each node holds exactly one live connection to the other.
    check(wait_until([&] { return ta.connections_open() == 1; }), "A holds one connection to B", ok);
    check(wait_until([&] { return tb.connections_open() == 1; }), "B holds one connection to A", ok);

    ta.stop();
    tb.stop();
    std::printf("tcp_transport_loopback_test: %s  (A→B recv=%llu, B→A recv=%llu)\n", ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(rb.count()), static_cast<unsigned long long>(ra.count()));
    return ok ? 0 : 1;
}
