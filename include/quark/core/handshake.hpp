// Implements ADR-040 §"Where the mTLS handshake runs" — the std-only HANDSHAKE seam, mirroring
// aead.hpp's adapter-seam pattern. `SecureTransport` (secure_transport.hpp) drives a `HandshakeEngine`
// as a step machine over `FrameKind::Authenticate` frames on the ALREADY-OPEN transport connection —
// no new public surface is added to the 010 `Transport`/`TcpTransport` for this. The real mTLS
// implementation (a TLS 1.3 mutual handshake over mbedTLS, directional exporter-derived traffic
// secrets) lives behind this seam in include/quark/adapters/mbedtls/; a `MockHandshakeEngine` test
// double (in tests/) proves the state machine and glare-free role assignment without linking mbedTLS.
//
// ROLE ASSIGNMENT: glare-free by construction. Both ends independently compute `is_client` from the
// same deterministic NodeId-ordering rule already used for TCP dial dedup (cluster.hpp's
// `keep_local_dial`) — no wire-level negotiation round-trip is needed (021 spec rec).
#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "quark/core/aead.hpp"
#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark {

class HandshakeEngine {
public:
    enum class Step { WantWrite, WantRead, Done, Failed };
    virtual ~HandshakeEngine() = default;

    // Advance the handshake. `in` is the next inbound chunk from the peer (empty on the client's
    // first call, which produces the opening message). Any bytes that must reach the peer are
    // APPENDED to `out_to_send` (may stay empty on a WantRead step).
    //   WantWrite — bytes were appended; send them, then wait for the peer's next message.
    //   WantRead  — nothing to send; block on the next inbound chunk.
    //   Done      — call take_result() once, then discard this engine.
    //   Failed    — call failure_reason() for diagnostics, then discard this engine.
    virtual Step advance(std::span<const std::byte> in, std::vector<std::byte>& out_to_send) = 0;

    struct Result {
        std::shared_ptr<const Aead> send_cipher;   // this side's write-direction key
        std::shared_ptr<const Aead> recv_cipher;   // this side's read-direction key
        Fingerprint peer_fingerprint{};             // SHA-256 of the peer's verified leaf cert
        NodeId peer_node_id{};                      // bound from the verified leaf cert (never claimed)
        ClusterId peer_cluster_id{};                // ditto — cross-checked against this node's own
    };
    // Valid only immediately after advance() returns Done. Consumes internal state; call once.
    [[nodiscard]] virtual Result take_result() = 0;

    // Valid only after advance() returns Failed.
    [[nodiscard]] virtual std::string_view failure_reason() const noexcept = 0;
};

// Constructs one HandshakeEngine per connection attempt. `is_client` comes from the caller's own
// glare-free role computation (see the file banner) — the factory does not decide it.
class HandshakeEngineFactory {
public:
    virtual ~HandshakeEngineFactory() = default;

    // `revoked` (may be null == nothing revoked yet) is checked against the peer's leaf fingerprint
    // DURING the handshake (S3: a revoked-but-unexpired cert is rejected before any session exists —
    // not just swept afterward by SecureTransport::sweep_revocations, which only covers ALREADY-open
    // sessions).
    [[nodiscard]] virtual std::unique_ptr<HandshakeEngine> create(
        bool is_client, NodeId self, NodeId expected_peer, ClusterId cluster_id,
        IdentityMaterial::Snapshot own_identity, TrustStore::Snapshot trust,
        std::shared_ptr<const std::unordered_set<Fingerprint>> revoked) = 0;
};

}  // namespace quark
