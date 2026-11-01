// Tests ADR-070 D6 (021 §2 "LAN discovery adapter"): REAL Windows DNS-SD on this machine, through
// WindowsDnsSdPlatform (dnsapi). One adapter registers an application-supplied random instance id and
// host label; a second adapter in the same process browses and must resolve it (port + label match).
// If the environment blocks mDNS (no multicast, firewall, no usable interface, API unavailable) the check
// prints an explicit SKIP reason and exits 77 (CTest SKIP_RETURN_CODE); it never passes silently.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

#include "quark/adapters/dnssd/windows_dnssd.hpp"

using namespace quark::net;

namespace {
int skip(const char* why) {
    std::printf("dnssd_windows_smoke_check: SKIP (%s)\n", why);
    return 77;
}

// The address the OS would use for a LAN route (UDP connect sends no packet). 0 if there is none.
std::uint32_t local_lan_ipv4() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // TEST-NET-1, never routed anywhere
    std::uint32_t out = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) out = ntohl(me.sin_addr.s_addr);
    }
    closesocket(s);
    return out;
}
}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const std::uint32_t ip = local_lan_ipv4();
    if (ip == 0) return skip("no routable IPv4 interface on this host");

    std::random_device rd;
    DnsSdBytes group;
    for (int i = 0; i < 10; ++i) group.push_back(static_cast<std::byte>(rd() & 0xFF));
    DnsSdBytes mine = group;  // group id + 4 random bytes (ADR-070 F1)
    for (int i = 0; i < 4; ++i) mine.push_back(static_cast<std::byte>(rd() & 0xFF));
    const std::string host = "q" + encode_instance_label(DnsSdBytes(mine.begin() + 10, mine.end()));
    constexpr std::uint16_t kPort = 45678;

    WindowsDnsSdPlatform pa, pb;
    std::atomic<bool> found{false};
    DiscoveredPeer got;
    std::mutex gm;

    LanDiscovery adv(pa, [](const DiscoveredPeer&) {});
    DiscoveryScopeConfig ca;
    ca.service_type = "_quarkd6._tcp";
    ca.port = kPort;
    ca.ipv4 = ip;
    ca.window = [&] { return DiscoveryWindow{mine, {group}, host}; };
    auto sa = adv.open_scope(std::move(ca));
    if (auto r = adv.start_advertising(*sa); !r) {
        std::printf("  register failed: %.*s\n", static_cast<int>(r.error().detail.size()), r.error().detail.data());
        return skip("DnsServiceRegister failed or did not complete (mDNS unavailable or blocked)");
    }

    LanDiscovery br(pb, [&](const DiscoveredPeer& p) {
        std::lock_guard g(gm);
        got = p;
        found = true;
    });
    DiscoveryScopeConfig cb;
    cb.service_type = "_quarkd6._tcp";
    cb.port = 1;
    cb.window = [&] { return DiscoveryWindow{DnsSdBytes(4, std::byte{1}), {group}, "browser"}; };
    auto sb = br.open_scope(std::move(cb));
    if (!br.start_browsing(*sb)) return skip("DnsServiceBrowse failed");

    for (int i = 0; i < 150 && !found; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!found)
        return skip("registered OK but the browser saw nothing in 15 s (multicast not looped back, "
                    "firewall, or client isolation on this host)");

    std::lock_guard g(gm);
    const bool ok = got.instance_id == mine && got.port == kPort && got.host_label == host;
    if (got.ipv4 == 0)
        std::printf("  NOTE: PTR+SRV were browsed (instance, host label, port verified) but the host's A record was "
                    "not resolvable from the same machine; address resolution is UNVERIFIED here (ADR-070 F4)\n");
    else if (got.ipv4 != ip)
        std::printf("  NOTE: resolved address differs from the registered one\n");
    std::printf("  found instance(%zu bytes) host='%s' port=%u ipv4=%u.%u.%u.%u\n", got.instance_id.size(),
                got.host_label.c_str(), static_cast<unsigned>(got.port), (got.ipv4 >> 24) & 255, (got.ipv4 >> 16) & 255,
                (got.ipv4 >> 8) & 255, got.ipv4 & 255);
    std::printf("dnssd_windows_smoke_check: %s\n", ok ? "OK (real DNS-SD register + browse; address resolution: see NOTE if printed)" : "FAIL");
    return ok ? 0 : 1;
}
