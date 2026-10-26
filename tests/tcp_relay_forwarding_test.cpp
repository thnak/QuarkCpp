// Tests ADR-048 relay side, shape B (issue #14): a node with set_relay_forwarding() forwards transit
// frames (to != self) byte-for-byte; the admit predicate gates it (fail-closed open-proxy guard); the
// origin must match the connection's identity; set_relay_via() gives an ordered failover relay list.
// Topology: spoke A and spoke B each know ONLY relay R (hub-and-spoke); A -> B must cross R.
#include <cstdint>
#include <cstdio>

#include "quark/core/aead.hpp"
#include "quark/core/node_authority.hpp"
#include "quark/core/secure_transport.hpp"
#include "quark/net/relay_discovery.hpp"
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

// End to end: A -> R -> B with SecureTransport at the two ENDS only. R forwards sealed bytes without
// holding a session, so the relay can neither read nor tamper; B opens them as if they came direct.
// Admission comes from a NodeAuthority allowlist (the cluster trust anchor), and revoking a node cuts
// the relay path.
void secure_via_relay(bool& ok) {
    const NodeId A{11}, R{12}, B{13};
    MockCipher cipher(0x14ULL);
    AllowlistNodeAuthority authority;
    authority.admit(A);
    authority.admit(B);
    net::TcpTransport tr(R, pal::ipv4_loopback, 0);
    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    SecureTransport sa(ta, cipher, A);
    SecureTransport sb(tb, cipher, B);
    Recorder rb;
    sb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    sa.on_receive([](MessageFrame) {});
    tr.on_receive([](MessageFrame) {});
    tr.set_allow_insecure_plaintext(true);  // the relay carries only already-sealed end-to-end frames
    tr.set_relay_forwarding(authority);
    check(tr.start() && ta.start() && tb.start(), "secure: listeners start", ok);
    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));
    ta.set_relay_via(B, {R});
    for (std::uint64_t i = 0; i < 30; ++i) sa.send(B, seq_frame(A, B, i));
    check(wait_until([&] { return rb.count() >= 30; }, 5000), "secure: 30 sealed frames opened at B", ok);
    check(sb.replays_rejected() == 0 && sb.tamper_rejected() == 0, "secure: no replay/tamper rejections", ok);
    {
        std::lock_guard<std::mutex> g(rb.m);
        for (std::size_t i = 0; i < rb.seqs.size(); ++i) check(rb.seqs[i] == i, "secure: FIFO across relay", ok);
    }
    authority.revoke(A);  // cold mutation; no frames in flight
    const auto refused = tr.relay_refused();
    sa.send(B, seq_frame(A, B, 99));
    check(wait_until([&] { return tr.relay_refused() > refused; }, 3000), "secure: revoked origin refused", ok);
    check(rb.count() == 30, "secure: revoked origin's frame not delivered", ok);
    ta.stop(); tb.stop(); tr.stop();
}

// Discovery: relays come from the gossiped CapabilityView, not static config.
void discovery(bool& ok) {
    const NodeId A{21}, R1{22}, R2{23}, B{24}, GHOST{25};
    const Endpoint e1 = loopback_endpoint(R1, 7001), e2 = loopback_endpoint(R2, 7002);
    const Endpoint eg = loopback_endpoint(GHOST, 7003);
    NodeCapabilities bad;  // flag but malformed endpoint
    bad.add(Flag{net::kRelayFlag});
    bad.add(Label{net::kRelayPortLabel, "99999"});
    // GHOST advertises relay but is not a live member ⇒ ignored. B has a malformed advert ⇒ ignored.
    auto view = make_capability_view(
        {A, R1, R2, B}, 1,
        {{R2, net::advertise_relay(NodeCapabilities{}, e2)},
         {R1, net::advertise_relay(NodeCapabilities{Label{"zone", "eu"}}, e1)},
         {B, bad},
         {GHOST, net::advertise_relay(NodeCapabilities{}, eg)}});
    const auto found = net::discover_relays(view, A);
    check(found.size() == 2 && found[0] == e1 && found[1] == e2,
          "discovery: live, well-formed relays in ascending NodeId order; ghost + malformed ignored", ok);
    check(net::discover_relays(view, R1).size() == 1, "discovery: a node never selects itself", ok);

    // Live: A and B know nothing but discovery; relay R forwards.
    const NodeId R{26};
    net::TcpTransport tr(R, pal::ipv4_loopback, 0);
    net::TcpTransport ta(NodeId{27}, pal::ipv4_loopback, 0);
    net::TcpTransport tb(NodeId{28}, pal::ipv4_loopback, 0);
    Recorder rb;
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    tr.on_receive([](MessageFrame) {});
    tr.set_relay_forwarding([](NodeId, NodeId) { return true; });
    check(tr.start() && ta.start() && tb.start(), "discovery: listeners start", ok);
    tr.add_peer(loopback_endpoint(NodeId{28}, tb.listen_port()));
    const Endpoint relay_ep = loopback_endpoint(R, tr.listen_port());
    auto live = make_capability_view({NodeId{27}, R, NodeId{28}}, 2,
                                     {{R, net::advertise_relay(NodeCapabilities{}, relay_ep)}});
    const NodeId targets[] = {NodeId{28}};
    net::apply_relay_discovery(ta, live, targets);
    net::apply_relay_discovery(tb, live, std::span<const NodeId>{});  // B only needs R reachable
    ta.send(NodeId{28}, seq_frame(NodeId{27}, NodeId{28}, 5));
    check(wait_until([&] { return rb.count() >= 1; }, 5000), "discovery: frame relayed via a gossip-discovered relay", ok);
    ta.stop(); tb.stop(); tr.stop();
}
}  // namespace

int main() {
    bool ok = true;
    discovery(ok);
    secure_via_relay(ok);
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
