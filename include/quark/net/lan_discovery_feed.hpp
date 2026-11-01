// Implements 021 §2 "LAN discovery adapter" (Draft) and ADR-070 §2 — the discovered-endpoint ->
// connection feed. A `DiscoveredPeer` is an ADDRESS, not an identity: the transport dials by NodeId
// (TcpTransport::add_peer), and the NodeId is only learned from the handshake. Until the pinned-key
// handshake (issue #40) lets the transport dial an address and learn who answered, the application
// supplies a `NodeResolver` (ADR-070 §4 finding F3). The feed only teaches the transport where a node
// listens; it never touches NodeAuthority, so discovery grants nothing (admission still decides).
#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "quark/core/cluster.hpp"
#include "quark/net/lan_discovery.hpp"
#include "quark/net/tcp_transport.hpp"

namespace quark::net {

using NodeResolver = std::function<std::optional<NodeId>(const DiscoveredPeer&)>;

// Returns a callback suitable for LanDiscovery's on_found. `t` must outlive the discovery adapter.
[[nodiscard]] inline LanDiscovery::FoundFn tcp_discovery_feed(TcpTransport& t, NodeResolver resolve) {
    return [&t, resolve = std::move(resolve)](const DiscoveredPeer& p) {
        if (auto n = resolve(p)) t.add_peer(Endpoint{*n, p.ipv4, p.port});
    };
}

}  // namespace quark::net
