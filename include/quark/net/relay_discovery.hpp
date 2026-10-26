// Implements ADR-048 §Amendment (issue #14) — relay DISCOVERY over ADR-045 capability gossip. A node
// that is willing to relay advertises a `relay` flag plus the endpoint it listens on; spokes derive an
// ordered relay list from the live CapabilityView instead of static config. Only current MEMBERS count
// (a gossiped capability for a node SWIM no longer lists is ignored), the list is ordered by NodeId so
// every spoke picks the same failover order, and it is bounded. The advertisement is gossip: it names
// where a relay LISTENS, it does not grant trust — whether a relay may forward for a peer is still the
// relay's own `set_relay_forwarding` admission, and end-to-end confidentiality/integrity is
// SecureTransport at the two ends.
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "quark/core/capabilities.hpp"
#include "quark/core/cluster.hpp"
#include "quark/net/tcp_transport.hpp"

namespace quark::net {

inline constexpr const char* kRelayFlag = "relay";
inline constexpr const char* kRelayAddrLabel = "relay_addr";  // decimal Endpoint::addr
inline constexpr const char* kRelayPortLabel = "relay_port";  // decimal Endpoint::port
inline constexpr std::size_t kMaxDiscoveredRelays = 8;

// What a relay-willing node publishes via CapabilityRegistry::publish_local(). `caps` may already hold
// other facts (zone, weight, ...) — the relay facts are appended.
[[nodiscard]] inline NodeCapabilities advertise_relay(NodeCapabilities caps, const Endpoint& listen) {
    caps.add(Flag{kRelayFlag});
    caps.add(Label{kRelayAddrLabel, std::to_string(listen.addr)});
    caps.add(Label{kRelayPortLabel, std::to_string(listen.port)});
    return caps;
}

namespace detail {
template <class T>
[[nodiscard]] inline bool parse_dec(std::string_view s, T& out) noexcept {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
}  // namespace detail

// The relays `self` can use right now: live members (not self) that advertise the relay flag with a
// well-formed endpoint, ascending NodeId, at most kMaxDiscoveredRelays. Pure function of the view.
[[nodiscard]] inline std::vector<Endpoint> discover_relays(const CapabilityView& view, NodeId self) {
    std::vector<Endpoint> out;
    for (NodeId n : view.nodes()) {  // ascending NodeId by MembershipView's invariant
        if (n == self) continue;
        const NodeCapabilities& c = view.capabilities_of(n);
        if (!c.has_flag(kRelayFlag)) continue;
        Endpoint ep{n, 0, 0};
        std::uint32_t port = 0;
        if (!detail::parse_dec(c.label(kRelayAddrLabel), ep.addr) ||
            !detail::parse_dec(c.label(kRelayPortLabel), port) || port == 0 || port > 0xFFFF)
            continue;
        ep.port = static_cast<std::uint16_t>(port);
        out.push_back(ep);
        if (out.size() == kMaxDiscoveredRelays) break;
    }
    return out;
}

// Apply a discovery result: teach `t` each relay's endpoint and make the relays the ordered fallback
// for every peer in `spokes_targets` that has no direct endpoint (shape B). Call again on each view
// change; idempotent. Targets that ARE relays are skipped (a relay is reached directly).
inline void apply_relay_discovery(TcpTransport& t, const CapabilityView& view,
                                  std::span<const NodeId> targets) {
    const std::vector<Endpoint> relays = discover_relays(view, t.self());
    if (relays.empty()) return;
    std::vector<NodeId> order;
    for (const Endpoint& r : relays) {
        t.add_peer(r);
        order.push_back(r.node);
    }
    for (NodeId peer : targets) {
        bool is_relay = false;
        for (NodeId r : order) is_relay = is_relay || r == peer;
        if (!is_relay) t.set_relay_via(peer, order);
    }
}

}  // namespace quark::net
