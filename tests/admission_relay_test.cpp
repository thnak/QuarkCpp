// Tests ADR-069 / 021 §"Application-driven eviction": the node-side relay (issue #14, TcpTransport::
// set_relay_forwarding(const NodeAuthority&)) consults the CURRENT admission snapshot on every transit
// frame, because AdmissionAuthority IS-A NodeAuthority: no relay path forwards a frame to or from a node
// the snapshot denies (or restricts to log exchange), and re-admission restores it. Real loopback sockets,
// topology as tcp_relay_forwarding_test: spoke A and spoke B know only relay R.
// CONTROL: while both are admitted the same path forwards (so the refusals below are the snapshot's doing).
#include <cstdio>

#include "admission_test_util.hpp"
#include "quark/net/tcp_transport.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

int main() {
    bool ok = true;
    const NodeId A{11}, R{12}, B{13};
    MockCipher cipher(0x14ULL);
    AdmissionAuthority authority;  // the relay node's own snapshot (no listener: TcpTransport reads it directly)
    (void)authority.publish(snap(1, {A, B}));

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
    check(tr.start() && ta.start() && tb.start(), "relay: listeners start", ok);
    ta.add_peer(loopback_endpoint(R, tr.listen_port()));
    tb.add_peer(loopback_endpoint(R, tr.listen_port()));
    tr.add_peer(loopback_endpoint(B, tb.listen_port()));
    ta.set_relay_via(B, {R});

    std::uint64_t seq = 0;
    for (int i = 0; i < 10; ++i) sa.send(B, seq_frame(A, B, seq++));
    check(wait_until([&] { return rb.count() >= 10; }, 5000), "relay CONTROL: both admitted, 10 frames forwarded", ok);

    auto expect_refused = [&](const char* what, std::uint64_t delivered_before) {
        const auto refused = tr.relay_refused();
        sa.send(B, seq_frame(A, B, seq++));
        check(wait_until([&] { return tr.relay_refused() > refused; }, 3000), what, ok);
        check(rb.count() == delivered_before, "relay: the refused frame is not delivered", ok);
    };

    (void)authority.publish(snap(2, {A}));  // destination B evicted
    expect_refused("relay: a frame TO an evicted destination is refused", 10);

    (void)authority.publish(snap(3, {A, B}));  // re-admitted
    sa.send(B, seq_frame(A, B, seq++));
    check(wait_until([&] { return rb.count() >= 11; }, 5000), "relay: re-admission restores forwarding", ok);

    (void)authority.publish(snap(4, {B}));  // origin A evicted
    expect_refused("relay: a frame FROM an evicted origin is refused", 11);

    (void)authority.publish(snap(5, {A}, {B}));  // B restricted to log exchange: not relayable
    expect_refused("relay: a log-exchange-only destination is not relayed to", 11);

    ta.stop();
    tb.stop();
    tr.stop();
    std::printf("admission_relay_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
