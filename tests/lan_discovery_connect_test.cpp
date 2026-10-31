// Tests ADR-070 D1 (021 §2 "LAN discovery adapter"): two nodes with NO seeds find each other through the
// DnsSdPlatform seam (in-memory fake) and then connect over real loopback TcpTransport sockets. The
// discovered endpoint only feeds add_peer; the transport dials it lazily. Control: with discovery
// not started the same nodes never learn an address, so nothing is delivered. Also pins finding F1:
// two members advertising the IDENTICAL instance id collide (mDNS names are unique), so the
// application must make the advertised bytes unique per member (group id + random suffix).
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

#include "lan_discovery_test_util.hpp"
#include "quark/net/lan_discovery_feed.hpp"
#include "tcp_test_util.hpp"

using namespace quark;
using namespace quark::test;

int main() {
    Check check;
    const NodeId A{1}, B{2};

    auto run = [&](bool discovery_on, std::uint64_t& received_by_b, std::size_t& seen_on_lan) {
        net::FakeDnsSdNetwork lan;
        net::TcpTransport ta(A, pal::ipv4_loopback, 0), tb(B, pal::ipv4_loopback, 0);
        ta.set_allow_insecure_plaintext(true);
        tb.set_allow_insecure_plaintext(true);
        Recorder ra, rb;
        ta.on_receive([&](MessageFrame f) { ra(std::move(f)); });
        tb.on_receive([&](MessageFrame f) { rb(std::move(f)); });
        check(ta.start() && tb.start(), "transports start");

        Node na(lan, 0xA1, ta.listen_port()), nb(lan, 0xB2, tb.listen_port());
        // Stand-in for "the handshake tells us who answered" (ADR-070 F3): map the advertised port to a node.
        auto resolver = [&](const DiscoveredPeer& p) -> std::optional<NodeId> {
            if (p.port == ta.listen_port()) return A;
            if (p.port == tb.listen_port()) return B;
            return std::nullopt;
        };
        net::LanDiscovery da(na.platform, net::tcp_discovery_feed(ta, resolver));
        net::LanDiscovery db(nb.platform, net::tcp_discovery_feed(tb, resolver));
        auto cfg = [&](Node& n, std::uint16_t port) {
            net::DiscoveryScopeConfig c;
            c.service_type = "_aerotest._tcp";
            c.port = port;
            c.ipv4 = static_cast<std::uint32_t>(pal::ipv4_loopback);
            c.window = [&n] { return n.make_window(); };
            return c;
        };
        auto sa = *da.open_scope(cfg(na, ta.listen_port()));
        auto sb = *db.open_scope(cfg(nb, tb.listen_port()));
        check(da.start_advertising(sa).has_value() && db.start_advertising(sb).has_value(), "advertise");
        if (discovery_on)
            check(da.start_browsing(sa).has_value() && db.start_browsing(sb).has_value(), "browse");
        // A third observer on the same fake LAN counts the adverts a browser can see.
        seen_on_lan = 0;
        if (discovery_on) {
            Node obs(lan, 0xC3, 1);
            obs.open();
            check(obs.disc.start_browsing(obs.scope).has_value(), "observer browse");
            seen_on_lan = obs.found_count();
        }
        ta.send(B, seq_frame(A, B, 7));
        wait_until([&] { return rb.count() >= 1; }, discovery_on ? 3000 : 400);
        received_by_b = rb.count();
        ta.stop();
        tb.stop();
    };

    std::uint64_t got_on = 0, got_off = 0;
    std::size_t obs_on = 0, obs_off = 0;
    run(true, got_on, obs_on);
    run(false, got_off, obs_off);
    check(got_on == 1, "discovery on: A reaches B with no seeds");
    check(obs_on == 2, "discovery on: the LAN shows both adverts");
    check(got_off == 0, "CONTROL discovery off: no address learned, nothing delivered");

    // F1: identical advertised ids collide on the network.
    {
        net::FakeDnsSdNetwork lan;
        net::FakeDnsSdPlatform p1(lan), p2(lan);
        net::DnsSdAdvert ad{"_aerotest._tcp", net::encode_instance_label(window_id(1)), "h1", 0, 9, {}};
        check(p1.register_service(ad).has_value(), "first member registers the group id");
        auto second = p2.register_service(ad);
        check(!second.has_value() && second.error().code == errc::validation,
              "F1: second member with the identical id is refused (name conflict)");
        ad.instance_label = net::encode_instance_label(with_suffix(window_id(1), 5));
        check(p2.register_service(ad).has_value(), "F1 remedy: id + random suffix registers");
    }

    std::printf("lan_discovery_connect_test: %s  (on=%llu off=%llu)\n", check.ok ? "OK" : "FAIL",
                static_cast<unsigned long long>(got_on), static_cast<unsigned long long>(got_off));
    return check.ok ? 0 : 1;
}
