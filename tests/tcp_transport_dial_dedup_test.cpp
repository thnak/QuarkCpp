// Tests 021 §2 dial deduplication REALIZED over real racing sockets (cluster_dial_dedup_test proves the
// pure rule; this proves the transport enforces it). When A and B dial each other simultaneously, two
// TCP connections form; the lower-NodeId-initiated one is kept and the other closed, so each node
// converges to EXACTLY ONE connection per peer (010's one-connection-per-peer invariant).
//
// #65: the race BURST itself must now be lossless, not just traffic sent after the dedup settles. The
// acceptor's DialHint keeps the dialer from confirming until the election is settled, so neither side
// writes an application frame on the connection that is about to lose. This test repeats the simultaneous
// dial many times and asserts every frame of the race burst arrives exactly once, in order.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

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

std::vector<std::uint64_t> snapshot(Recorder& r) {
    std::lock_guard<std::mutex> g(r.m);
    return r.seqs;
}

// Every sequence 0..n-1 exactly once, in order.
bool is_exact_run(Recorder& r, std::uint64_t n) {
    const std::vector<std::uint64_t> got = snapshot(r);
    if (got.size() != n) return false;
    for (std::uint64_t i = 0; i < n; ++i)
        if (got[static_cast<std::size_t>(i)] != i) return false;
    return true;
}
}  // namespace

int main() {
    bool ok = true;
    const NodeId A{1}, B{2};
    constexpr int kRounds = 50;
    constexpr std::uint64_t kBurst = 5;

    int raced_rounds = 0;
    for (int round = 0; round < kRounds; ++round) {
        net::TcpTransport ta(A, pal::ipv4_loopback, 0);
        net::TcpTransport tb(B, pal::ipv4_loopback, 0);
        Recorder ra, rb;
        ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
        tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
        check(ta.start() && tb.start(), "both listeners start", ok);
        if (!ok) break;
        ta.add_peer(loopback_endpoint(B, tb.listen_port()));
        tb.add_peer(loopback_endpoint(A, ta.listen_port()));

        // Force the concurrency hazard: both nodes send (⇒ dial) at the same instant, so two connections
        // race into existence. Sending from two threads makes the first dials genuinely concurrent.
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::thread sender_a([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < kBurst; ++i) ta.send(B, seq_frame(A, B, i));
        });
        std::thread sender_b([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (std::uint64_t i = 0; i < kBurst; ++i) tb.send(A, seq_frame(B, A, i));
        });
        while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        sender_a.join();
        sender_b.join();

        // THE 010/021 INVARIANT: each node converges to EXACTLY ONE connection to its peer.
        check(wait_until([&] { return ta.connections_open() == 1; }), "A converges to ONE connection to B",
              ok);
        check(wait_until([&] { return tb.connections_open() == 1; }), "B converges to ONE connection to A",
              ok);
        // #65: and the burst that raced with the first dials is delivered exactly once, in order.
        check(wait_until([&] { return ra.count() >= kBurst && rb.count() >= kBurst; }),
              "the race burst arrives at both ends", ok);
        check(is_exact_run(ra, kBurst), "B->A race burst: exactly 0..4, in order", ok);
        check(is_exact_run(rb, kBurst), "A->B race burst: exactly 0..4, in order", ok);
        if (ta.dedup_closed() + tb.dedup_closed() > 0) ++raced_rounds;

        // Post-settle: the surviving single connection carries a full batch both ways with NO loss.
        const std::uint64_t a_before = ra.count(), b_before = rb.count();
        constexpr std::uint64_t M = 200;
        for (std::uint64_t i = 0; i < M; ++i) {
            ta.send(B, seq_frame(A, B, 1000 + i));
            tb.send(A, seq_frame(B, A, 2000 + i));
        }
        check(wait_until([&] { return rb.count() >= b_before + M && ra.count() >= a_before + M; }),
              "the surviving connection carries a full batch both ways, losslessly, after dedup", ok);
        check(ta.connections_open() == 1 && tb.connections_open() == 1,
              "still exactly one connection per peer after post-settle traffic", ok);

        ta.stop();
        tb.stop();
        if (!ok) break;
    }

    // The harness must actually have exercised the race, not just 50 quiet connections.
    check(raced_rounds > 0, "at least one simultaneous-dial race occurred across the rounds", ok);

    std::printf("tcp_transport_dial_dedup_test: %s  (%d/%d rounds raced)\n", ok ? "OK" : "FAIL",
                raced_rounds, kRounds);
    return ok ? 0 : 1;
}
