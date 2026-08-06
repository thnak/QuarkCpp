// Tests ADR-046's S4 fix on the real socket transport: a `FrameKind::Control` frame must not be
// head-of-line-blocked behind a peer's own `FrameKind::Data` backlog on the shared per-peer
// connection (021: one connection per peer, unchanged — no new socket). TcpTransport's per-peer
// queue (`outq_`/`Conn::out`) is split control/data with control-ahead-of-data write priority
// (tcp_transport.hpp `pump()`); this proves it end to end over real 127.0.0.1 sockets, mirroring
// tcp_transport_loopback_test.cpp's harness style.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
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

// A frame carrying `seq` in trace_id (arrival-order marker) and a payload of `bytes` size, of the
// given FrameKind — big Data payloads are what builds a real backlog; the single Control frame
// carries a tiny payload, as a real Congested control message would.
MessageFrame big_frame(NodeId from, NodeId to, std::uint64_t seq, FrameKind kind, std::size_t bytes) {
    MessageFrame f;
    f.from = from;
    f.to = to;
    f.target = ActorId{TypeKey{0x5151u}, seq};
    f.msg_type = TypeKey{0x99u};
    f.mode = WireMode::Tagged;
    f.trace_id = seq;
    f.kind = kind;
    f.payload.assign(bytes, static_cast<std::byte>(seq & 0xFF));
    return f;
}
}  // namespace

int main() {
    bool ok = true;
    const NodeId A{1}, B{2};
    constexpr std::uint64_t kDataFrames = 500;
    constexpr std::size_t kFrameBytes = 16 * 1024;       // 16 KiB/frame ⇒ ~8 MiB total backlog
    constexpr std::uint64_t kControlSeq = kDataFrames;   // sent LAST, must not arrive LAST

    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);

    std::mutex order_mu;
    std::vector<std::uint64_t> arrival_order;   // trace_id in arrival order
    std::vector<FrameKind> arrival_kind;
    std::atomic<std::uint64_t> received{0};
    tb.on_receive([&](MessageFrame f) {
        {
            std::lock_guard<std::mutex> g(order_mu);
            arrival_order.push_back(f.trace_id);
            arrival_kind.push_back(f.kind);
        }
        received.fetch_add(1, std::memory_order_release);
        // Throttle B's consumption (on B's OWN io thread, which is also the thread doing the kernel
        // recv() calls) well below loopback's native throughput — without this, a modern kernel's
        // auto-tuned socket buffers happily absorb the whole 8 MiB backlog before A's writes ever hit
        // would_block, leaving nothing genuinely queued in A's `Conn::out` for the priority fix to
        // reorder (a real, disclosed limitation of testing this over a real, very fast local loop).
        // Sleeping here forces genuine TCP receive-window backpressure onto A's sender side, which is
        // exactly the "slow/congested peer" condition ADR-046's S4 fix targets. How MUCH backlog this
        // actually produces in `Conn::out` at the moment the control frame is queued is itself
        // platform/kernel-buffer-dependent (observed: MSVC/Windows loopback ~30/500, Linux ~300-350/500
        // under this exact throttle) — the assertion below is deliberately loose to stay robust across
        // that variance while still ruling out "arrives dead last" (the pre-fix, single-FIFO behavior).
        if (f.kind == FrameKind::Data) std::this_thread::sleep_for(std::chrono::microseconds(1500));
    });
    ta.on_receive([](MessageFrame) {});

    check(ta.start(), "node A listener starts", ok);
    check(tb.start(), "node B listener starts", ok);
    if (!ok) {
        std::printf("tcp_transport_control_priority_test: FAIL (startup)\n");
        return 1;
    }
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));

    // Queue the ENTIRE data backlog first, then one Control frame — worst case for HOL blocking if
    // the queue were a single undifferentiated FIFO (the pre-ADR-046 behavior).
    for (std::uint64_t i = 0; i < kDataFrames; ++i)
        ta.send(B, big_frame(A, B, i, FrameKind::Data, kFrameBytes));
    ta.send(B, big_frame(A, B, kControlSeq, FrameKind::Control, 64));

    check(wait_until([&] { return received.load(std::memory_order_acquire) >= kDataFrames + 1; }, 30000),
          "B eventually receives every data frame plus the control frame", ok);

    std::size_t control_index = arrival_order.size();
    {
        std::lock_guard<std::mutex> g(order_mu);
        for (std::size_t i = 0; i < arrival_order.size(); ++i) {
            if (arrival_kind[i] == FrameKind::Control) {
                control_index = i;
                break;
            }
        }
    }
    check(control_index < arrival_order.size(), "the control frame was actually observed", ok);
    // Queued dead last behind the whole backlog; control-ahead-of-data priority means it must not
    // land at (or near) the very tail — the threshold is intentionally loose (kept well short of
    // kDataFrames) to stay robust across platforms' differing kernel-buffer/backlog characteristics
    // while still ruling out the pre-fix single-FIFO behavior (which lands it at EXACTLY the tail).
    check(control_index < kDataFrames * 3 / 4,
          "control frame arrives meaningfully before the tail, not stuck dead last behind the backlog",
          ok);

    std::printf(
        "tcp_transport_control_priority_test: %s  (control arrived at index %zu of %zu)\n",
        ok ? "OK" : "FAIL", control_index, arrival_order.size());

    ta.stop();
    tb.stop();
    return ok ? 0 : 1;
}
