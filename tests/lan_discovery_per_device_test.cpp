// Tests ADR-070 D7/D8/D9 (021 §2 "LAN discovery adapter"; AeroCoWorker ADR-044 rule R16, which resolves
// ADR-070 F1): every device advertises its OWN rotating id and a browser accepts the EXACT ids of the
// other devices, current window (accept_ids) and previous window (accept_previous_ids).
//   D7: 50 nodes in one scope, 50 distinct ids, each accepts the other 49: every node finds exactly 49
//       peers, never itself, zero name conflicts. Control: 50 nodes advertising ONE shared id: the fake
//       refuses 49 registrations (the F1 failure R16 removes).
//   D8: exact match. X is reported; X+1 byte, X-last byte, and X with one bit flipped are not. Control:
//       a test-local copy of the OLD prefix rule accepts the X+1-byte label.
//   D9: previous_window is true iff the match came from accept_previous_ids (current wins when an id is
//       in both lists); after window_boundary the old ids stop matching new events.
#include <algorithm>
#include <cstdio>
#include <memory>

#include "lan_discovery_test_util.hpp"

using namespace quark;
using namespace quark::test;

namespace {
constexpr const char* kType = "_aerotest._tcp";
constexpr unsigned kN = 50;

DnsSdBytes ten(unsigned seed) {
    DnsSdBytes b;
    for (unsigned i = 0; i < 10; ++i) b.push_back(static_cast<std::byte>((seed * 29u + i * 7u + 3u) & 0xFF));
    return b;
}

// The OLD rule, kept only here as the D8 control: accept when the decoded label STARTS WITH an id.
bool old_prefix_accepts(const DnsSdBytes& label, const DnsSdBytes& accepted) {
    return !accepted.empty() && label.size() >= accepted.size() &&
           std::equal(accepted.begin(), accepted.end(), label.begin());
}

net::DnsSdSighting sighting(const DnsSdBytes& id, std::uint16_t port) {
    return net::DnsSdSighting{kType, net::encode_instance_label(id), "host", 0, port};
}

// A platform whose browse hands the test its callback, so a test can inject arbitrary sightings.
class InjectPlatform final : public net::DnsSdPlatform {
public:
    result<net::DnsSdHandle> register_service(const net::DnsSdAdvert&) override { return net::DnsSdHandle{1}; }
    void deregister_service(net::DnsSdHandle) override {}
    result<net::DnsSdHandle> browse(std::string_view, std::function<void(const net::DnsSdEvent&)> cb) override {
        cb_ = std::move(cb);
        return net::DnsSdHandle{2};
    }
    void stop_browse(net::DnsSdHandle) override {}
    void inject(const net::DnsSdSighting& s) { cb_({net::DnsSdEventKind::Found, s}); }

private:
    std::function<void(const net::DnsSdEvent&)> cb_;
};

struct Harness {
    explicit Harness(DiscoveryWindow w0)
        : win(std::move(w0)), disc(plat, [this](const DiscoveredPeer& p) { found.push_back(p); }) {
        net::DiscoveryScopeConfig c;
        c.service_type = kType;
        c.port = 1;
        c.window = [this] { return win; };
        scope = *disc.open_scope(std::move(c));
        (void)disc.start_browsing(scope);
    }
    DiscoveryWindow win;
    InjectPlatform plat;
    std::vector<DiscoveredPeer> found;
    net::LanDiscovery disc;
    net::ScopeId scope = 0;
};

struct Member {
    std::unique_ptr<net::FakeDnsSdPlatform> plat;
    std::unique_ptr<net::LanDiscovery> disc;
    std::vector<DiscoveredPeer> found;
    net::ScopeId scope = 0;
};

struct Lan50 {
    net::FakeDnsSdNetwork lan;
    std::vector<std::unique_ptr<Member>> m;
    unsigned registered = 0;
    // distinct: node i advertises ten(i) and accepts the other 49. Otherwise all share ten(1000).
    explicit Lan50(bool distinct) {
        for (unsigned i = 0; i < kN; ++i) {
            auto n = std::make_unique<Member>();
            n->plat = std::make_unique<net::FakeDnsSdPlatform>(lan);
            Member* raw = n.get();
            n->disc = std::make_unique<net::LanDiscovery>(*n->plat,
                                                          [raw](const DiscoveredPeer& p) { raw->found.push_back(p); });
            net::DiscoveryScopeConfig c;
            c.service_type = kType;
            c.port = static_cast<std::uint16_t>(3000 + i);
            c.window = [i, distinct] {
                DiscoveryWindow w;
                w.advertise_id = distinct ? ten(i) : ten(1000);
                for (unsigned j = 0; j < kN; ++j)
                    if (j != i) w.accept_ids.push_back(distinct ? ten(j) : ten(1000));
                w.host_label = "h" + std::to_string(i);
                return w;
            };
            n->scope = *n->disc->open_scope(std::move(c));
            m.push_back(std::move(n));
        }
        for (auto& n : m) registered += n->disc->start_advertising(n->scope).has_value();
        for (auto& n : m) (void)n->disc->start_browsing(n->scope);
    }
};
}  // namespace

int main() {
    Check check;

    // ---- D7 ----
    {
        Lan50 a(true);
        bool all49 = true, never_self = true;
        std::size_t lo = ~std::size_t{0}, hi = 0;
        for (unsigned i = 0; i < kN; ++i) {
            const auto& f = a.m[i]->found;
            all49 &= f.size() == kN - 1;
            lo = std::min(lo, f.size());
            hi = std::max(hi, f.size());
            for (auto& p : f) never_self &= p.port != 3000 + i;
        }
        check(a.registered == kN, "D7: all 50 distinct ids register (zero name conflicts)");
        check(all49, "D7: every node finds exactly 49 peers");
        check(never_self, "D7: no node reports itself");
        std::printf("  D7: registered=%u/%u, found per node min=%zu max=%zu (expected 49), live=%zu\n", a.registered, kN,
                    lo, hi, a.lan.live_registrations());

        Lan50 c(false);
        std::size_t total = 0;
        for (auto& n : c.m) total += n->found.size();
        check(c.registered == 1 && kN - c.registered == 49,
              "D7 control: with one shared id the fake refuses 49 registrations");
        std::printf("  D7 control: shared id registered=%u refused=%u, peers found by all nodes in total=%zu\n",
                    c.registered, kN - c.registered, total);
    }

    // ---- D8 ----
    {
        const DnsSdBytes x = ten(7);
        DnsSdBytes plus = x;
        plus.push_back(std::byte{0x5A});
        DnsSdBytes minus(x.begin(), x.end() - 1);
        DnsSdBytes flip = x;
        flip[4] ^= std::byte{0x10};
        DiscoveryWindow w;
        w.advertise_id = ten(99);
        w.accept_ids = {x};
        w.host_label = "me";
        Harness h(w);
        h.plat.inject(sighting(plus, 11));
        h.plat.inject(sighting(minus, 12));
        h.plat.inject(sighting(flip, 13));
        const std::size_t wrong = h.found.size();
        h.plat.inject(sighting(x, 14));
        check(wrong == 0, "D8: X+1 byte, X-last byte and X with a flipped bit are NOT reported");
        check(h.found.size() == 1 && h.found[0].port == 14 && h.found[0].instance_id == x,
              "D8: X itself is reported");
        const bool ctrl = old_prefix_accepts(plus, x);
        check(ctrl, "D8 control: the OLD prefix rule accepts the X+1-byte label");
        check(!old_prefix_accepts(minus, x) && !old_prefix_accepts(flip, x),
              "D8 control: the old rule still rejects the shorter and the flipped label");
        std::printf("  D8: reported for wrong labels=%zu (expected 0), for X=%zu (expected 1); old prefix rule accepts X+1byte=%d\n",
                    wrong, h.found.size(), ctrl);
    }

    // ---- D9 ----
    {
        const DnsSdBytes cur = ten(21), prev = ten(22), both = ten(23), later = ten(24);
        DiscoveryWindow w;
        w.advertise_id = ten(99);
        w.accept_ids = {cur, both, DnsSdBytes{}};
        w.accept_previous_ids = {prev, both, DnsSdBytes{}};
        w.host_label = "me";
        Harness h(w);
        h.plat.inject(sighting(prev, 31));
        h.plat.inject(sighting(cur, 32));
        h.plat.inject(sighting(both, 33));
        check(h.found.size() == 3, "D9: three sightings reported");
        auto flag = [&](std::uint16_t port) {
            for (auto& p : h.found)
                if (p.port == port) return p.previous_window;
            return false;
        };
        const bool f_prev = flag(31), f_cur = flag(32), f_both = flag(33);
        check(f_prev, "D9: accept_previous_ids match -> previous_window true");
        check(!f_cur, "D9: accept_ids match -> previous_window false");
        check(!f_both, "D9: id in both lists -> current wins, false");
        // Boundary: the application now hands a new result; the old ids stop matching.
        h.win.accept_ids = {later};
        h.win.accept_previous_ids = {cur};
        check(h.disc.window_boundary(h.scope).has_value(), "D9: window boundary");
        const std::size_t before = h.found.size();
        h.plat.inject(sighting(prev, 41));
        h.plat.inject(sighting(both, 42));
        h.plat.inject(sighting(later, 43));
        h.plat.inject(sighting(cur, 44));
        const std::size_t gained = h.found.size() - before;
        check(gained == 2, "D9: after the boundary only the new ids match (old prev and both stop matching)");
        check(gained == 2 && h.found[before].port == 43 && !h.found[before].previous_window &&
                  h.found[before + 1].port == 44 && h.found[before + 1].previous_window,
              "D9: new current id false, old current id now previous (true)");
        std::printf("  D9: previous_window prev=%d cur=%d both=%d; after boundary 4 events (2 stale, 2 new) -> matched=%zu (expected 2)\n",
                    f_prev, f_cur, f_both, gained);
    }

    std::printf("lan_discovery_per_device_test: %s\n", check.ok ? "OK" : "FAIL");
    return check.ok ? 0 : 1;
}
