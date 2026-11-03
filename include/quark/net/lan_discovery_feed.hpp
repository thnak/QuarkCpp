// Implements 021 §2 "LAN discovery adapter" (Draft) and ADR-070 §2 — the discovered-endpoint ->
// connection feed. A `DiscoveredPeer` is an ADDRESS, not an identity. Two feeds: `tcp_discovery_feed` is the
// HELLO-wire path (the transport dials by NodeId, so the application supplies a `NodeResolver`, ADR-070 F3);
// `secure_discovery_feed` (#57) is the pinned-wire path, which dials the address itself and installs a session
// only for the key the roster expected there. Neither touches NodeAuthority, so discovery grants nothing
// (admission still decides).
#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "quark/core/cluster.hpp"
#include "quark/core/secure_transport.hpp"
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

// #57 (ADR-070 F3, ADR-072 §5): the pinned-wire feed. The application's roster says which peer an advertisement
// belongs to (`expected`, a NodeId derived from the key it pinned); the endpoint is DIALLED, never recorded as that
// peer's address, and a session exists only if the handshake authenticates that key. An advertisement the roster does
// not know (nullopt) is ignored; one pointing at an attacker's listener gets a Msg1 encrypted to the expected key and
// nothing else. Discovery still grants nothing: admission decides. `s` must outlive the discovery adapter.
using ExpectedPeerFn = std::function<std::optional<NodeId>(const DiscoveredPeer&)>;

[[nodiscard]] inline LanDiscovery::FoundFn secure_discovery_feed(SecureTransport& s, ExpectedPeerFn expected) {
    return [&s, expected = std::move(expected)](const DiscoveredPeer& p) {
        if (const auto n = expected(p)) s.open_address(*n, p.ipv4, p.port);
    };
}

}  // namespace quark::net
