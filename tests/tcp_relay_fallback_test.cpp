// Tests ADR-048's DirectThenRelay<TimeoutMs> fallback policy over real sockets: a peer whose direct
// endpoint never answers is, after `fallback_timeout_ns`, redialed through its set_relay()-configured
// Endpoint instead — and once the direct endpoint later becomes reachable, an out-of-band probe recovers
// the peer back to a direct connection with a fenced cutover (no frame reordered or duplicated across
// the swap). This test stands in for a real relay component (out of this ADR's adopted scope — see
// ADR-048's non-goals) by pointing set_relay() straight at B's own real listener: from TcpTransport's own
// state-machine perspective this exercises exactly the mechanism the ADR describes (dial-target
// resolution, one-shot give-up timer, out-of-band recovery probe, fenced fd swap) without needing a
// byte-transparent relay process to actually stand up.
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

// Reserve an ephemeral loopback port and immediately release it, so a connect() to it fails fast
// (connection refused) instead of hanging — the "direct endpoint nobody is listening on yet" fixture.
std::uint16_t reserve_free_port() {
    auto fd = pal::tcp_listen(pal::ipv4_loopback, 0);
    const std::uint16_t port = *pal::local_port(*fd);
    pal::close_fd(*fd);
    return port;
}
}  // namespace

int main() {
    bool ok = true;
    const NodeId A{1}, B{2};
    constexpr std::int64_t fallback_ns = 150'000'000;  // 150 ms — small enough for a fast test

    // `relay` stands in for B's designated relay Endpoint: B's OWN real listener (see file banner — this
    // ADR's adopted scope is the client-side fallback state machine, not a relay-forwarding component).
    net::TcpTransport relay(B, pal::ipv4_loopback, 0);
    Recorder r_relay;
    relay.on_receive([&](MessageFrame f) { r_relay(std::move(f)); });
    check(relay.start(), "relay-standin listener starts", ok);

    const std::uint16_t direct_port = reserve_free_port();  // nothing listens here yet ⇒ fast refusal

    net::TcpTransport ta(A, pal::ipv4_loopback, 0, fallback_ns);
    check(ta.start(), "A's listener starts", ok);
    if (!ok) {
        std::printf("tcp_relay_fallback_test: FAIL (startup)\n");
        return 1;
    }
    ta.add_peer(loopback_endpoint(B, direct_port));               // direct: unreachable
    ta.set_relay(B, loopback_endpoint(B, relay.listen_port()));   // relay: reachable

    // --- Phase 1: direct times out, A falls back to the relay Endpoint -------------------------------
    ta.send(B, seq_frame(A, B, 0));
    check(wait_until([&] { return r_relay.count() >= 1; }, 5000),
          "frame delivered via the relay Endpoint after the direct give-up timeout", ok);

    for (std::uint64_t i = 1; i < 5; ++i) ta.send(B, seq_frame(A, B, i));
    check(wait_until([&] { return r_relay.count() >= 5; }, 3000),
          "subsequent frames continue flowing over the relay Endpoint", ok);

    // --- Phase 2: B becomes reachable at its direct Endpoint ⇒ A recovers ----------------------------
    net::TcpTransport direct(B, pal::ipv4_loopback, direct_port);  // same NodeId B, now listening directly
    Recorder r_direct;
    direct.on_receive([&](MessageFrame f) { r_direct(std::move(f)); });
    check(direct.start(), "B's direct listener comes online", ok);

    // Keep sending while the recovery probe (armed every fallback_ns) finds the now-live direct endpoint
    // and cuts over. total sent tracks every frame so the final loss/duplication check is exact.
    constexpr std::uint64_t total = 40;
    for (std::uint64_t i = 5; i < total; ++i) {
        ta.send(B, seq_frame(A, B, i));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(wait_until([&] { return r_direct.count() >= 1; }, 5000),
          "at least one frame recovers onto the direct connection after cutover", ok);
    check(wait_until([&] { return r_relay.count() + r_direct.count() >= total; }, 3000),
          "every sent frame is eventually delivered (via relay, then direct)", ok);

    // --- Correctness: no duplication, no reordering across the cutover -------------------------------
    check(r_relay.seqs.size() + r_direct.seqs.size() == total, "no duplicated or extra deliveries", ok);
    auto strictly_increasing = [](const std::vector<std::uint64_t>& v) {
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i] <= v[i - 1]) return false;
        return true;
    };
    check(strictly_increasing(r_relay.seqs), "relay-leg deliveries arrive in send order", ok);
    check(strictly_increasing(r_direct.seqs), "direct-leg deliveries (post-cutover) arrive in send order",
          ok);
    if (!r_relay.seqs.empty() && !r_direct.seqs.empty()) {
        check(r_relay.seqs.back() < r_direct.seqs.front(),
              "the cutover is a clean split — every relay-delivered seq precedes every direct-delivered "
              "seq (no in-flight frame was reordered across the swap)",
              ok);
    }

    std::printf(
        "tcp_relay_fallback_test: %s  (relay recv=%zu, direct recv=%zu, total sent=%llu)\n",
        ok ? "OK" : "FAIL", r_relay.seqs.size(), r_direct.seqs.size(),
        static_cast<unsigned long long>(total));

    ta.stop();
    relay.stop();
    direct.stop();
    return ok ? 0 : 1;
}
