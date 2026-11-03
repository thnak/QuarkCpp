// #57 (ADR-070 F3, ADR-072 §5): a discovered endpoint is dialled BY ADDRESS on the pinned wire. The roster (the application)
// says which peer an advertisement belongs to; the transport dials the address, records nothing about it, and a session exists
// only for the key that answers. Over real loopback sockets, TcpTransport + SecureTransport + the Noise IK engine, with the
// in-memory DnsSdPlatform as the LAN. Each property has a control:
//   D1 two nodes, no seeds, no add_peer, no NodeResolver: discovery brings both sessions up and frames flow both ways.
//      CONTROL: discovery off, they never connect.
//   D2 an advertisement that points at an attacker's listener yields no session; the listener receives exactly one Msg1
//      (encrypted to the EXPECTED key: the real responder's engine accepts it; it carries no key or NodeId of the dialler),
//      even though the attacker answers with a Msg2-sized blob. CONTROL: the same advertisement with the real address works.
//   D3 an advertisement the roster does not know is never dialled (zero connections).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "pinned_tcp_util.hpp"
#include "quark/net/lan_discovery.hpp"
#include "quark/net/lan_discovery_fake.hpp"
#include "quark/net/lan_discovery_feed.hpp"

using namespace quark;
using namespace quark::adapters;
using namespace quark::test;

namespace {

net::DnsSdBytes dev_id(unsigned n) {  // a 10-byte per-device id (ADR-044 R16 shape); the test plays the application
    net::DnsSdBytes b;
    for (unsigned i = 0; i < 10; ++i) b.push_back(static_cast<std::byte>((n * 37u + i * 11u + 5u) & 0xFF));
    return b;
}

constexpr const char* kService = "_aerotest._tcp";

// One discovery endpoint on the fake LAN: advertises `own`, accepts `accept`, and feeds `on_found`.
struct Disc {
    Disc(net::FakeDnsSdNetwork& lan, unsigned own, std::vector<unsigned> accept, std::uint16_t port,
         net::LanDiscovery::FoundFn on_found)
        : platform(lan), disc(platform, std::move(on_found)) {
        net::DiscoveryScopeConfig c;
        c.service_type = kService;
        c.port = port;
        c.ipv4 = static_cast<std::uint32_t>(pal::ipv4_loopback);
        c.window = [own, accept] {
            net::DiscoveryWindow w;
            w.advertise_id = dev_id(own);
            for (unsigned a : accept) w.accept_ids.push_back(dev_id(a));
            w.host_label = "h" + std::to_string(own);
            return w;
        };
        scope = *disc.open_scope(std::move(c));
    }
    bool advertise() { return disc.start_advertising(scope).has_value(); }
    bool browse() { return disc.start_browsing(scope).has_value(); }
    net::FakeDnsSdPlatform platform;
    net::LanDiscovery disc;
    net::ScopeId scope = 0;
};

// The application's roster: advertised id -> the peer it belongs to.
using Roster = std::map<std::vector<std::byte>, NodeId>;
net::ExpectedPeerFn roster_fn(std::shared_ptr<Roster> r) {
    return [r](const net::DiscoveredPeer& p) -> std::optional<NodeId> {
        const auto it = r->find(p.instance_id);
        if (it == r->end()) return std::nullopt;
        return it->second;
    };
}

}  // namespace

int main() {
    bool ok = true;
    if (!noise::init()) return 1;
    auto check = [&](bool c, const char* what) {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    };

    // ---- D1: no seeds, no resolver --------------------------------------------------------------------
    for (const bool discovery_on : {true, false}) {
        Node n1, n2;
        Node& lo = n1.id.value < n2.id.value ? n1 : n2;  // names follow the ids: the lower id is the designated initiator
        Node& hi = n1.id.value < n2.id.value ? n2 : n1;
        pair_up(lo, hi, kPinnedTcpCluster);
        Stack lo_s(lo, /*pinned_wire=*/true), hi_s(hi, true);
        check(lo_s.start() && hi_s.start(), "D1: pinned listeners start");

        auto roster = std::make_shared<Roster>();
        (*roster)[dev_id(1)] = lo.id;  // device 1 is lo, device 2 is hi
        (*roster)[dev_id(2)] = hi.id;
        net::FakeDnsSdNetwork lan;
        Disc d_lo(lan, 1, {2}, lo_s.tcp.listen_port(), net::secure_discovery_feed(lo_s.sec, roster_fn(roster)));
        Disc d_hi(lan, 2, {1}, hi_s.tcp.listen_port(), net::secure_discovery_feed(hi_s.sec, roster_fn(roster)));
        check(d_lo.advertise() && d_hi.advertise(), "D1: both advertise");
        if (discovery_on) check(d_lo.browse() && d_hi.browse(), "D1: both browse");

        if (discovery_on) {
            check(wait_for([&] { return lo_s.sec.session_count() == 1 && hi_s.sec.session_count() == 1; }, 4000),
                  "D1: discovery alone brings a session up on both ends");
            check(deliver(lo_s, hi_s, hi.id, 1, "lo->hi"), "D1: the dialler's frame is delivered");
            check(deliver(hi_s, lo_s, lo.id, 1, "hi->lo"), "D1: the answerer's frame is delivered");
        } else {
            check(!deliver(lo_s, hi_s, hi.id, 1, "no discovery", 1200), "D1 CONTROL: discovery off, nothing is delivered");
            check(lo_s.sec.session_count() == 0 && hi_s.sec.session_count() == 0,
                  "D1 CONTROL: discovery off, no session on either end");
        }
    }

    // ---- D2/D3: a fake advertisement, and one nobody knows ---------------------------------------------
    {
        Node n1, n2;
        Node& lo = n1.id.value < n2.id.value ? n1 : n2;
        Node& hi = n1.id.value < n2.id.value ? n2 : n1;
        pair_up(lo, hi, kPinnedTcpCluster);
        Stack lo_s(lo, true);  // the dialler; `hi` is only the identity the roster expects behind the fake advert
        check(lo_s.start(), "D2: dialler starts");

        std::mutex m;
        std::vector<std::byte> seen;  // everything the attacker's listener received
        auto attacker_handler = [&]() -> ConnHandler {
            auto replied = std::make_shared<bool>(false);
            return [&m, &seen, replied](Bytes& received, Bytes& reply) -> Verdict {
                {
                    std::lock_guard g(m);
                    seen = received;
                }
                if (!*replied && received.size() >= kPinnedMsg1Len) {  // answer with a Msg2-sized blob once Msg1 is in
                    *replied = true;
                    reply.assign(kPinnedMsg2Len, std::byte{0x5A});
                }
                return Verdict::Continue;
            };
        };
        TcpResponder attacker(attacker_handler);
        TcpResponder untouched([]() -> ConnHandler { return [](Bytes&, Bytes&) { return Verdict::Continue; }; });

        auto roster = std::make_shared<Roster>();
        (*roster)[dev_id(2)] = hi.id;  // the roster knows device 2 only; device 3 is unknown to it
        net::FakeDnsSdNetwork lan;
        Disc d_lo(lan, 1, {2, 3}, lo_s.tcp.listen_port(), net::secure_discovery_feed(lo_s.sec, roster_fn(roster)));
        // The fake advertisements: device 2's id (the roster's `hi`) at the ATTACKER's port; device 3 at another listener.
        Disc fake_hi(lan, 2, {}, attacker.port(), {});
        Disc unknown(lan, 3, {}, untouched.port(), {});
        check(fake_hi.advertise() && unknown.advertise(), "D2: the fake advertisements are up");
        check(d_lo.browse(), "D2: the dialler browses");

        check(wait_for([&] { return attacker.connections() >= 1; }, 3000), "D2: the fake advertisement is dialled");
        check(wait_for([&] {
                  std::lock_guard g(m);
                  return seen.size() >= kPinnedMsg1Len;
              }, 3000),
              "D2: the attacker's listener receives a Msg1");
        sleep_ms(500);  // give a (wrong) session time to appear
        check(lo_s.sec.session_count() == 0, "D2: an attacker's listener yields no session");
        std::vector<std::byte> got;
        {
            std::lock_guard g(m);
            got = seen;
        }
        // Exactly one Authenticate frame: header + the 154-byte Msg1 payload, nothing more, nothing earlier.
        check(got.size() > kPinnedMsg1Len && got.size() <= net::TcpTransport::kAnonOpenerMax,
              "D2: the attacker sees one opener frame and nothing beyond it");
        const Bytes msg1(got.end() - static_cast<std::ptrdiff_t>(kPinnedMsg1Len), got.end());
        check(!contains(got, lo.pub) && !contains(got, le64(lo.id.value)) && !contains(got, le64(hi.id.value)),
              "D2: the Msg1 shows neither key nor any NodeId (it is encrypted to the expected key)");
        auto real_responder = hi.engine(false, NodeId{0}, kPinnedTcpCluster);
        const auto fed = feed_responder(*real_responder, msg1);
        check(fed.step != HandshakeEngine::Step::Failed,
              "D2: the Msg1 is exactly what the EXPECTED node accepts (it was encrypted to that node's key)");
        check(untouched.connections() == 0, "D3: an advertisement the roster does not know is never dialled");
    }

    // D2 CONTROL: the same roster entry pointing at the real node's address is served.
    {
        Node n1, n2;
        Node& lo = n1.id.value < n2.id.value ? n1 : n2;
        Node& hi = n1.id.value < n2.id.value ? n2 : n1;
        pair_up(lo, hi, kPinnedTcpCluster);
        Stack lo_s(lo, true), hi_s(hi, true);
        check(lo_s.start() && hi_s.start(), "D2 CONTROL: listeners start");
        auto roster = std::make_shared<Roster>();
        (*roster)[dev_id(2)] = hi.id;
        net::FakeDnsSdNetwork lan;
        Disc d_lo(lan, 1, {2}, lo_s.tcp.listen_port(), net::secure_discovery_feed(lo_s.sec, roster_fn(roster)));
        Disc real_hi(lan, 2, {}, hi_s.tcp.listen_port(), {});
        check(real_hi.advertise() && d_lo.browse(), "D2 CONTROL: advertise and browse");
        check(deliver(lo_s, hi_s, hi.id, 1, "control"), "D2 CONTROL: the real address, same roster entry, gets a session");
    }

    std::printf("pinned_discovery_dial_check: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
