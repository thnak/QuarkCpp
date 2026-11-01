// Tests ADR-070 D4 and the per-scope runtime control (021 §2 "LAN discovery adapter"): stop advertising
// and a FRESH browser finds nothing; an existing browser is told the peer went away. Control: before the
// stop a fresh browser does find it. stop_browsing silences events; several scopes advertise at once
// with different ids and service types; close_scope withdraws only its own advert; the adapter's
// destructor withdraws everything (nothing left on the LAN). Client isolation: nothing is found and the
// adapter still reports Unknown (it cannot tell isolation from an empty network, ADR-070 section 5).
#include <cstdio>

#include "lan_discovery_test_util.hpp"

using namespace quark;
using namespace quark::test;

int main() {
    Check check;
    net::FakeDnsSdNetwork lan;
    Node a(lan, 0xA1, 2001);
    a.open();
    check(a.disc.start_advertising(a.scope).has_value() && a.disc.advertising(a.scope), "A advertises");

    Node watcher(lan, 0xE5, 2005);
    watcher.open();
    check(watcher.disc.start_browsing(watcher.scope).has_value(), "watcher browses");
    check(watcher.found_count() == 1, "watcher sees A");

    {
        Node fresh_before(lan, 0xF6, 2006);
        fresh_before.open();
        (void)fresh_before.disc.start_browsing(fresh_before.scope);
        check(fresh_before.found_count() == 1, "CONTROL: a fresh browser BEFORE the stop finds A");
    }

    a.disc.stop_advertising(a.scope);
    check(!a.disc.advertising(a.scope) && lan.live_registrations() == 0, "A stopped: nothing registered on the LAN");
    {
        Node fresh_after(lan, 0xF7, 2007);
        fresh_after.open();
        (void)fresh_after.disc.start_browsing(fresh_after.scope);
        check(fresh_after.found_count() == 0, "D4: a fresh browser AFTER the stop finds nothing");
    }
    {
        std::lock_guard g(watcher.m);
        check(watcher.lost.size() == 1, "existing browser is told A went away");
    }

    // stop_browsing silences a node.
    watcher.disc.stop_browsing(watcher.scope);
    check(!watcher.disc.browsing(watcher.scope), "watcher not browsing");
    (void)a.disc.start_advertising(a.scope);
    check(watcher.found_count() == 1, "stopped browser receives nothing new");

    // Several scopes at once, different ids and types.
    {
        net::FakeDnsSdNetwork lan2;
        net::FakeDnsSdPlatform p(lan2);
        net::LanDiscovery d(p, [](const DiscoveredPeer&) {});
        auto mk = [&](const char* type, unsigned w) {
            net::DiscoveryScopeConfig c;
            c.service_type = type;
            c.port = 7;
            c.window = [w] { return DiscoveryWindow{window_id(w), {window_id(w)}, "host" + std::to_string(w)}; };
            return *d.open_scope(std::move(c));
        };
        auto s1 = mk("_one._tcp", 1), s2 = mk("_two._tcp", 2);
        (void)d.start_advertising(s1);
        (void)d.start_advertising(s2);
        check(lan2.live_registrations() == 2, "two scopes advertise at once");
        const auto log = lan2.adverts_seen();
        check(log.size() == 2 && log[0].instance_label != log[1].instance_label, "...with different ids");
        d.close_scope(s1);
        check(lan2.live_registrations() == 1, "close_scope withdraws only its own advert");
    }
    {
        net::FakeDnsSdNetwork lan3;
        {
            net::FakeDnsSdPlatform p(lan3);
            net::LanDiscovery d(p, [](const DiscoveredPeer&) {});
            net::DiscoveryScopeConfig c;
            c.service_type = "_x._tcp";
            c.window = [] { return DiscoveryWindow{window_id(1), {window_id(1)}, "h"}; };
            auto s = *d.open_scope(std::move(c));
            (void)d.start_advertising(s);
            check(lan3.live_registrations() == 1, "advertising");
        }
        check(lan3.live_registrations() == 0, "adapter destruction withdraws every advert");
    }
    check(a.disc.visibility(a.scope) == net::NetworkVisibility::Unknown, "visibility is Unknown");
    {
        net::FakeDnsSdNetwork iso;
        iso.set_isolated(true);
        Node x(iso, 1, 1), y(iso, 2, 2);
        x.open();
        y.open();
        (void)x.disc.start_advertising(x.scope);
        (void)y.disc.start_browsing(y.scope);
        check(y.found_count() == 0 && y.disc.visibility(y.scope) == net::NetworkVisibility::Unknown,
              "client isolation: nothing found AND the adapter still says Unknown");
    }

    std::printf("lan_discovery_stop_test: %s\n", check.ok ? "OK" : "FAIL");
    return check.ok ? 0 : 1;
}
