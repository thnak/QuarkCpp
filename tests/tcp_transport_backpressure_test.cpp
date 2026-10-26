// Outbound backlog bounding of the default TcpTransport (review C1 — REVIEW-BACKLOG.md).
// A peer that never completes the hello (stuck in Connecting / Backoff, or a slow reader) used to let
// the per-peer outbound queue grow without bound while the app kept sending. Now:
//   1. Data frames are shed once a peer's queued bytes would pass the data cap (counted, drops());
//   2. Control-class frames (SWIM, handshake) still queue past the data cap, under their own hard cap;
//   3. byte accounting never drifts (push / pop / salvage / close_peer all balance — audited on the loop);
//   4. after the backlog is torn down (close_peer) the cap is available again (no leaked counters,
//      including the cross-thread "posted but not yet encoded" counter).
// The "slow peer" is a raw listening socket that accepts connections in the kernel but never speaks.
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

constexpr std::uint64_t kAny = pal::ipv4(0, 0, 0, 0);

MessageFrame sized_frame(NodeId from, NodeId to, std::uint64_t seq, std::size_t payload, FrameKind kind) {
    MessageFrame f = seq_frame(from, to, seq);
    f.payload.assign(payload, std::byte{0x5A});
    f.kind = kind;
    return f;
}
}  // namespace

int main() {
    bool ok = true;
    const NodeId A{1}, B{2};

    auto lfd = pal::tcp_listen(pal::ipv4_loopback, 0);
    check(static_cast<bool>(lfd), "stub listener opens", ok);
    if (!lfd) return 1;
    const auto stub_port = pal::local_port(*lfd);
    check(static_cast<bool>(stub_port), "stub port readable", ok);
    if (!stub_port) return 1;

    constexpr std::uint64_t kDataCap = 64 * 1024;
    constexpr std::uint64_t kCtlCap = 256 * 1024;
    constexpr std::size_t kPayload = 1000;
    constexpr int kN = 500;  // ~500 KB offered against a 64 KiB cap

    net::TcpTransport t(A, kAny, 0);
    t.set_max_outbound_data_bytes(kDataCap);
    t.set_max_outbound_control_bytes(kCtlCap);
    t.add_peer(loopback_endpoint(B, *stub_port));
    check(t.start(), "transport starts", ok);

    // ---- 1. data shed at the cap ----------------------------------------------------------------
    for (int i = 0; i < kN; ++i) t.send(B, sized_frame(A, B, static_cast<std::uint64_t>(i), kPayload, FrameKind::Data));
    check(wait_until([&] { return t.frames_sent() + t.outbound_data_drops() == kN; }, 5000),
          "every offered data frame is either queued or counted as shed", ok);
    const std::uint64_t queued = t.outbound_queued_bytes();
    std::fprintf(stderr, "  queued=%llu data_drops=%llu\n", static_cast<unsigned long long>(queued),
                 static_cast<unsigned long long>(t.outbound_data_drops()));
    check(t.outbound_data_drops() > 0, "data frames past the cap are shed", ok);
    check(queued <= kDataCap, "queued data bytes never exceed the cap", ok);
    check(queued > 0, "some data was queued", ok);
    check(t.drops() >= t.outbound_data_drops(), "sheds are folded into drops()", ok);
    check(t.outq_accounting_consistent(), "byte accounting consistent after shedding", ok);

    // ---- 2. control-class frames still flow past a full data lane --------------------------------
    const std::uint64_t before_ctl = t.frames_sent();
    for (int i = 0; i < 20; ++i)
        t.send(B, sized_frame(A, B, 1000u + static_cast<unsigned>(i), 100, i % 2 ? FrameKind::Control : FrameKind::Authenticate));
    check(wait_until([&] { return t.frames_sent() == before_ctl + 20; }, 5000),
          "control-class frames are queued even though the data cap is hit", ok);
    check(t.outbound_control_drops() == 0, "no control-class frame was shed under the hard cap", ok);
    check(t.outq_accounting_consistent(), "byte accounting consistent with control frames queued", ok);

    // ---- 3. control hard cap is finite ------------------------------------------------------------
    for (int i = 0; i < 600; ++i) t.send(B, sized_frame(A, B, 2000u + static_cast<unsigned>(i), 1000, FrameKind::Control));
    check(wait_until([&] { return t.outbound_control_drops() > 0; }, 5000),
          "control-class frames are shed at their (much larger) hard cap", ok);
    check(t.outbound_queued_bytes() <= kCtlCap + kDataCap, "control backlog is bounded too", ok);
    check(t.outq_accounting_consistent(), "byte accounting consistent after control shedding", ok);

    // ---- 4. teardown releases everything; the cap is reusable -------------------------------------
    t.close_peer(B);
    check(wait_until([&] { return t.outbound_queued_bytes() == 0; }, 5000),
          "close_peer drops the whole backlog (no leaked bytes)", ok);
    check(t.outq_accounting_consistent(), "byte accounting consistent after teardown", ok);
    t.add_peer(loopback_endpoint(B, *stub_port));  // (peer table still has it; harmless re-add)
    const std::uint64_t drops_before = t.outbound_data_drops();
    t.send(B, sized_frame(A, B, 9000, kPayload, FrameKind::Data));
    check(wait_until([&] { return t.outbound_queued_bytes() > 0 || t.frames_sent() > 0; }, 2000),
          "post-teardown send is accepted (posted-bytes counter did not leak)", ok);
    check(t.outbound_data_drops() == drops_before, "post-teardown send was not shed", ok);

    t.stop();
    pal::close_fd(*lfd);
    std::fprintf(stderr, ok ? "tcp_transport_backpressure_test: OK\n" : "tcp_transport_backpressure_test: FAILED\n");
    return ok ? 0 : 1;
}
