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
        hi_s.dial_via(lo.id, lo_s.tcp.listen_port());  // the requester knows the peer's endpoint

        check(deliver(hi_s, lo_s, lo.id, 1, "higher opens"), "the HIGHER id opens the session");
        check(deliver(lo_s, hi_s, hi.id, 1, "reply"), "the reply reaches the higher id");
        check(lo_s.sec.session_count() == 1 && hi_s.sec.session_count() == 1,
              "exactly one session on each end");
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
