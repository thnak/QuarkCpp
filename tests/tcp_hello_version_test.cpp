// Tests issue #20 / ADR-053: the TCP HELLO version window. The 14-byte hello layout and the current
// version (1) are unchanged; what changes is the acceptance rule: a peer within one version of ours is
// accepted (N and N-1, and N+1 so a rolling upgrade works in both directions), anything further is
// REFUSED with a distinct reason - observable through the transport's refusal handler and counter, and
// bridged into membership as a VersionRefused event (SwimMembership::note_transport_refusal).
// Three real-socket transports at versions 5 (N), 4 (N-1) and 3 (N-2).
#include <cstdint>
#include <cstdio>
#include <mutex>
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
struct Refusals {
    std::mutex m;
    std::vector<net::HelloRefusal> v;
    void add(const net::HelloRefusal& r) {
        std::lock_guard<std::mutex> g(m);
        v.push_back(r);
    }
    bool has(NodeId n, std::uint16_t ver, net::HelloVerdict why) {
        std::lock_guard<std::mutex> g(m);
        for (const auto& r : v)
            if (r.claimed_node == n && r.peer_version == ver && r.reason == why) return true;
        return false;
    }
};
}  // namespace

int main() {
    bool ok = true;
    check(net::hello_version == 1, "the current hello version is unchanged (byte-compatible)", ok);
    check(net::hello_size == 14, "the hello layout is unchanged", ok);

    const NodeId A{1}, B{2}, C{3};
    net::TcpTransport ta(A, pal::ipv4_loopback, 0);
    net::TcpTransport tb(B, pal::ipv4_loopback, 0);
    net::TcpTransport tc(C, pal::ipv4_loopback, 0);
    ta.set_hello_version(5);
    tb.set_hello_version(4);
    tc.set_hello_version(3);

    Recorder ra, rb, rc;
    ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
    tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
    tc.on_receive([&](MessageFrame f) { rc(std::move(f)); });

    // A bridges its refusals into a membership instance (membership state is untouched by the bridge
    // until the driving thread ticks - the bridge is thread-safe).
    LoopbackFabric fab;
    LoopbackTransport lt(fab, A);
    SwimMembership::Config cfg;
    cfg.cluster_id = ClusterId{0x7C9};
    SwimMembership swim(A, lt, cfg);
    std::vector<MembershipEvent> ev;
    swim.add_event_sink([&](const MembershipEvent& e) { ev.push_back(e); });

    Refusals refA, refC;
    ta.set_hello_refusal_handler([&](const net::HelloRefusal& r) {
        refA.add(r);
        swim.note_transport_refusal(r.claimed_node, r.peer_version, r.reason == net::HelloVerdict::VersionTooOld);
    });
    tc.set_hello_refusal_handler([&](const net::HelloRefusal& r) { refC.add(r); });

    check(ta.start() && tb.start() && tc.start(), "listeners start", ok);
    if (!ok) {
        std::printf("tcp_hello_version_test: FAIL (startup)\n");
        return 1;
    }
    ta.add_peer(loopback_endpoint(B, tb.listen_port()));
    ta.add_peer(loopback_endpoint(C, tc.listen_port()));
    tb.add_peer(loopback_endpoint(A, ta.listen_port()));
    tc.add_peer(loopback_endpoint(A, ta.listen_port()));

    // N <-> N-1: accepted, frames flow both ways.
    ta.send(B, seq_frame(A, B, 1));
    check(wait_until([&] { return rb.count() >= 1; }), "N -> N-1: frame delivered", ok);
    tb.send(A, seq_frame(B, A, 2));
    check(wait_until([&] { return ra.count() >= 1; }), "N-1 -> N: frame delivered", ok);
    check(tb.hello_version_refusals() == 0, "N-1 refused nothing", ok);

    // N <-> N-2: refused, with the distinct reason on each side.
    ta.send(C, seq_frame(A, C, 3));
    check(wait_until([&] { return refA.has(C, 3, net::HelloVerdict::VersionTooOld); }),
          "N refuses N-2 with VersionTooOld (peer version 3)", ok);
    check(wait_until([&] { return refC.has(A, 5, net::HelloVerdict::VersionTooNew); }),
          "N-2 refuses N with VersionTooNew (peer version 5)", ok);
    check(ta.hello_version_refusals() > 0 && tc.hello_version_refusals() > 0, "both sides count the refusal", ok);
    check(rc.count() == 0, "no frame was delivered across the refused link", ok);

    // Bridge: the refusal surfaces as a membership VersionRefused event on the driving thread's next tick.
    check(ev.empty(), "nothing is delivered off-thread (events wait for the driving thread)", ok);
    swim.tick();
    bool saw = false;
    for (const auto& e : ev)
        if (e.kind == MembershipEventKind::VersionRefused && e.node == C && e.refusal == RefusalReason::WireVersionTooOld &&
            e.protocol_version == 3)
            saw = true;
    check(saw, "transport refusal surfaces as VersionRefused(WireVersionTooOld) within one tick", ok);
    check(swim.health(C) && swim.health(C)->last_refusal == RefusalReason::WireVersionTooOld,
          "health() carries the transport refusal", ok);

    ta.stop();
    tb.stop();
    tc.stop();
    std::printf("tcp_hello_version_test: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
