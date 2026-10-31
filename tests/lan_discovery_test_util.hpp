// Shared helpers for the ADR-070 LAN discovery tests (NOT a *_test.cpp). A `Node` bundles a fake
// platform handle, an application-owned rotating window (the test plays the application: it owns the
// "key" and the clock), a LanDiscovery adapter, and a thread-safe record of what it found.
#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quark/net/lan_discovery.hpp"
#include "quark/net/lan_discovery_fake.hpp"

namespace quark::test {

using quark::net::DiscoveredPeer;
using quark::net::DiscoveryWindow;
using quark::net::DnsSdBytes;

inline DnsSdBytes bytes_of(std::initializer_list<unsigned> v) {
    DnsSdBytes b;
    for (unsigned x : v) b.push_back(static_cast<std::byte>(x));
    return b;
}
// A 10-byte stand-in for HMAC-SHA256(group key, window)[0..10): the test, not Quark, "owns the key".
inline DnsSdBytes window_id(unsigned window) {
    DnsSdBytes b;
    for (unsigned i = 0; i < 10; ++i) b.push_back(static_cast<std::byte>((window * 37u + i * 11u + 5u) & 0xFF));
    return b;
}
inline DnsSdBytes with_suffix(DnsSdBytes id, unsigned s) {
    for (unsigned i = 0; i < 4; ++i) id.push_back(static_cast<std::byte>((s >> (8 * i)) & 0xFF));
    return id;
}

struct Check {
    bool ok = true;
    void operator()(bool c, const char* what) {
        if (!c) {
            std::fprintf(stderr, "  CHECK FAILED: %s\n", what);
            ok = false;
        }
    }
};

struct Node {
    Node(net::FakeDnsSdNetwork& lan, unsigned suffix, std::uint16_t port, std::string type = "_aerotest._tcp")
        : platform(lan), suffix_(suffix), port_(port), type_(std::move(type)),
          disc(platform, [this](const DiscoveredPeer& p) {
              std::lock_guard g(m);
              found.push_back(p);
          }, [this](const DiscoveredPeer& p) {
              std::lock_guard g(m);
              lost.push_back(p);
          }) {}

    // The application's window provider: current + (optionally) previous window ids.
    DiscoveryWindow make_window() const {
        DiscoveryWindow w;
        w.advertise_id = with_suffix(window_id(cur), suffix_);
        w.accept_ids.push_back(window_id(cur));
        if (accept_previous && cur > 0) w.accept_ids.push_back(window_id(cur - 1));
        w.host_label = "h" + std::to_string(cur) + "x" + std::to_string(suffix_);
        return w;
    }
    net::ScopeId open() {
        net::DiscoveryScopeConfig c;
        c.service_type = type_;
        c.port = port_;
        c.window = [this] { return make_window(); };
        auto r = disc.open_scope(std::move(c));
        scope = *r;
        return scope;
    }
    std::size_t found_count() {
        std::lock_guard g(m);
        return found.size();
    }

    net::FakeDnsSdPlatform platform;
    unsigned cur = 1;
    bool accept_previous = true;
    net::ScopeId scope = 0;
    std::mutex m;
    std::vector<DiscoveredPeer> found, lost;

private:
    unsigned suffix_;
    std::uint16_t port_;
    std::string type_;

public:
    net::LanDiscovery disc;
};

}  // namespace quark::test
