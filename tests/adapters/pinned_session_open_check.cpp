// ADR-081 / #83: either end of an admitted pinned pair may open the session. The HIGHER NodeId calls send()
// first; it asks the transport for a bare connection, the accepting (designated-initiator) end binds it and
// opens the Noise IK handshake, and frames flow both ways. Control: without the accept-side hook nothing is
// sent by either end and no session forms.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "pinned_tcp_util.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    };

    // ---- the higher id opens ---------------------------------------------------------------------
    {
        std::vector<std::unique_ptr<Node>> nodes;
        for (int i = 0; i < 2; ++i) nodes.push_back(std::make_unique<Node>());
        std::sort(nodes.begin(), nodes.end(), [](auto& x, auto& y) { return x->id.value < y->id.value; });
        Node& lo = *nodes[0];
        Node& hi = *nodes[1];
        pair_up(lo, hi, kPinnedTcpCluster);

        Stack lo_s(lo, /*pinned_wire=*/true);
        Stack hi_s(hi, /*pinned_wire=*/true);
        lo_s.sec.set_inbound_session_peer(hi.id);  // the acceptor is the designated initiator
        check(lo_s.start() && hi_s.start(), "both pinned listeners start");

        // #79: session lifecycle hooks and the pre-session drop counter.
        std::atomic<int> up_lo{0}, up_hi{0}, down_hi{0};
        std::atomic<std::uint64_t> gen_lo{0};
        std::atomic<int> down_reason{-1};
        lo_s.sec.set_session_up_hook([&](NodeId p, std::uint64_t gen) {
            if (p == hi.id) { gen_lo.store(gen); up_lo.fetch_add(1); }
        });
        hi_s.sec.set_session_up_hook([&](NodeId p, std::uint64_t) {
            if (p == lo.id) up_hi.fetch_add(1);
        });
        hi_s.sec.set_session_down_hook([&](NodeId p, SecureTransport::SessionEndReason r) {
            if (p == lo.id) { down_reason.store(static_cast<int>(r)); down_hi.fetch_add(1); }
        });

        hi_s.dial_via(lo.id, lo_s.tcp.listen_port());  // the requester knows the peer's endpoint
        hi_s.sec.send(lo.id, data(hi.id, lo.id, "pre-session"));
        check(hi_s.sec.dropped_no_session(lo.id) == 1, "a pre-session send is counted per peer");

        hi_s.sec.open(lo.id);  // #79: explicit open, no frame
        check(wait_for([&] { return lo_s.sec.session_count() == 1 && hi_s.sec.session_count() == 1; }, 3000),
              "open() brings the session up without a frame");
        check(deliver(hi_s, lo_s, lo.id, 1, "higher opens"), "the HIGHER id opens the session");
        check(up_lo.load() == 1 && up_hi.load() == 1, "on_session_up fires once on each end");
        check(gen_lo.load() == 0, "the up hook reports the admission generation (0 when admission is off)");
        check(deliver(lo_s, hi_s, hi.id, 1, "reply"), "the reply reaches the higher id");
        check(lo_s.sec.session_count() == 1 && hi_s.sec.session_count() == 1,
              "exactly one session on each end");

        hi_s.sec.on_peer_disconnected(lo.id);  // #79: the down hook names the reason
        check(down_hi.load() == 1 &&
                  down_reason.load() == static_cast<int>(SecureTransport::SessionEndReason::PeerClosed),
              "on_session_down reports PeerClosed when a live session's connection dies");
        check(hi_s.sec.session_count() == 0, "the session is gone after the disconnect");
    }

    // ---- control: no accept-side hook => nothing opens -------------------------------------------
    {
        Node a2, b2;
        Node& lo2 = a2.id.value < b2.id.value ? a2 : b2;  // the names must follow the ids, not declaration order
        Node& hi2 = a2.id.value < b2.id.value ? b2 : a2;
        pair_up(lo2, hi2, kPinnedTcpCluster);
        Stack lo_s(lo2, true), hi_s(hi2, true);
        check(lo_s.start() && hi_s.start(), "CONTROL: listeners start");
        hi_s.dial_via(lo2.id, lo_s.tcp.listen_port());
        check(!deliver(hi_s, lo_s, lo2.id, 1, "no hook", 1200),
              "CONTROL: without the accept-side hook the higher id cannot open a session");
        check(lo_s.sec.session_count() == 0 && hi_s.sec.session_count() == 0,
              "CONTROL: no session is installed");
    }

    std::printf("pinned_session_open_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
