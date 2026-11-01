// Tests ADR-070 D5 (021 §2 "LAN discovery adapter"): discovery grants nothing. Node 3 is found on the
// LAN and fed into the transport (add_peer), yet the NodeAuthority (the existing AllowlistNodeAuthority;
// no dependency on issue #41) still refuses it: admitted(3) stays false and admission-gated placement
// never selects it, across many actor ids, even with node 3 live in the membership view. The feed
// holds no NodeAuthority, so it cannot admit. Control: once the authority admits node 3, placement
// DOES select it, so the exclusion above was the authority's doing, not an artefact.
#include <cstdio>
#include <memory>
#include <optional>

#include "lan_discovery_test_util.hpp"
#include "quark/core/node_authority.hpp"
#include "quark/net/lan_discovery_feed.hpp"

using namespace quark;
using namespace quark::test;

int main() {
    Check check;
    net::FakeDnsSdNetwork lan;
    const NodeId N1{1}, N2{2}, N3{3};

    net::TcpTransport t(N1, pal::ipv4_loopback, 0);
    t.set_allow_insecure_plaintext(true);
    Node stranger(lan, 0x33, 3333), me(lan, 0x11, 1111);
    stranger.open();
    (void)stranger.disc.start_advertising(stranger.scope);

    std::size_t fed = 0;
    auto feed = net::tcp_discovery_feed(t, [&](const DiscoveredPeer& p) -> std::optional<NodeId> {
        if (p.port == 3333) {
            ++fed;
            return N3;
        }
        return std::nullopt;
    });
    net::LanDiscovery d(me.platform, feed);
    net::DiscoveryScopeConfig c;
    c.service_type = "_aerotest._tcp";
    c.port = 1111;
    c.window = [&] { return me.make_window(); };
    auto s = *d.open_scope(std::move(c));
    (void)d.start_browsing(s);
    check(fed == 1, "node 3 was discovered and fed to the transport");

    AllowlistNodeAuthority authority;
    authority.admit(N1);
    authority.admit(N2);
    check(!authority.admitted(N3), "after discovery, node 3 is still NOT admitted");

    auto nodes = std::make_shared<std::vector<NodeId>>(std::vector<NodeId>{N1, N2, N3});
    const MembershipView view(nodes, 1);
    std::size_t picked3 = 0;
    for (std::uint64_t k = 0; k < 2000; ++k) {
        auto w = place_admitted(ActorId{TypeKey{0x77u}, k}, view, authority);
        if (w && *w == N3) ++picked3;
    }
    check(picked3 == 0, "admission-gated placement never selects the discovered, unadmitted node");

    authority.admit(N3);
    std::size_t picked3_after = 0;
    for (std::uint64_t k = 0; k < 2000; ++k) {
        auto w = place_admitted(ActorId{TypeKey{0x77u}, k}, view, authority);
        if (w && *w == N3) ++picked3_after;
    }
    check(picked3_after > 0, "CONTROL: once the authority admits node 3, placement selects it");
    std::printf("  placements onto node 3: unadmitted=%zu admitted=%zu (of 2000)\n", picked3, picked3_after);

    std::printf("lan_discovery_grants_nothing_test: %s\n", check.ok ? "OK" : "FAIL");
    return check.ok ? 0 : 1;
}
