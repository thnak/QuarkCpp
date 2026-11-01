// Shared helpers for the ADR-070 LAN discovery tests (NOT a *_test.cpp). A `Node` bundles a fake
// platform handle, an application-owned rotating window (the test plays the application: it owns the
// "key" and the clock), a LanDiscovery adapter, and a thread-safe record of what it found.
#pragma once

#include <cstddef>
#include <algorithm>
#include <cstdio>
#include <map>
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
// ADR-044 R16: every device has its OWN id per window (10 bytes: the window stand-in with the last four
// bytes replaced by the device's number). No shared prefix, no suffix; browsers match it EXACTLY.
inline DnsSdBytes device_id(unsigned window, unsigned device) {
    DnsSdBytes id = window_id(window);
    for (unsigned i = 0; i < 4; ++i) id[6 + i] = static_cast<std::byte>((device >> (8 * i)) & 0xFF);
    return id;
}
// The application's knowledge of the other active devices on one fake LAN (keyed by the LAN).
inline std::vector<unsigned>& known_devices(const void* lan) {
    static std::map<const void*, std::vector<unsigned>> reg;
    return reg[lan];
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
        : platform(lan), lan_(&lan), suffix_(suffix), port_(port), type_(std::move(type)),
          disc(platform, [this](const DiscoveredPeer& p) {
              std::lock_guard g(m);
              found.push_back(p);
          }, [this](const DiscoveredPeer& p) {
              std::lock_guard g(m);
              lost.push_back(p);
          }) {
        auto& k = known_devices(lan_);
        if (std::find(k.begin(), k.end(), suffix_) == k.end()) k.push_back(suffix_);
    }

    // The application's window provider: current + (optionally) previous window ids.
    DiscoveryWindow make_window() const {
        DiscoveryWindow w;
        w.advertise_id = device_id(cur, suffix_);
        for (unsigned d : known_devices(lan_)) {
            if (d == suffix_) continue;
            w.accept_ids.push_back(device_id(cur, d));
            if (accept_previous && cur > 0) w.accept_previous_ids.push_back(device_id(cur - 1, d));
        }
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
    const void* lan_;
    unsigned suffix_;
    std::uint16_t port_;
    std::string type_;

public:
    net::LanDiscovery disc;
};

}  // namespace quark::test
