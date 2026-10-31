// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" and ADR-068 — the std-only, libsodium-ignorant TYPES of the
// pinned-key trust mode: the identity (an Ed25519 public key), the application's trust provider, and the key directory
// the initiator uses to find the key it pins for a peer. The cryptography (Noise IK over libsodium) lives behind the
// HandshakeEngine seam in include/quark/adapters/sodium/; the core stays std-only per 020's "honest exception".
//
// THE TRUST DECISION IS THE APPLICATION'S. Quark holds no CA and no static key file in this mode: at EVERY handshake it
// asks the application's `PinnedTrust` whether `key` is admitted to `cluster` right now. The provider must be
// thread-safe and non-blocking (it is called from the transport's I/O path); consulting it fresh each time is what makes
// "un-admit" take effect on the NEXT handshake. Re-checking sessions that are ALREADY open when trust changes is #41
// (application-driven admission) and is NOT done here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "quark/core/ids.hpp"

namespace quark {

// How a cluster/engine authenticates its peers. Selected per cluster by which HandshakeEngineFactory is installed in
// SecureTransport::enable_handshake: CaChain = the mbedTLS mTLS factory (ADR-040), PinnedKey = the Noise IK factory
// (ADR-068). The value is bound into the Noise prologue, so a CaChain peer and a PinnedKey peer can never complete a
// handshake with each other — there is no negotiation to downgrade.
enum class TrustMode : std::uint8_t { CaChain = 0, PinnedKey = 1 };

// A peer's identity in pinned mode: its FULL Ed25519 public key (32 bytes), never a hash or a truncation. The NodeId is
// DERIVED from it (adapter: first 8 bytes of SHA-256 of this key), never self-claimed.
using Ed25519PublicKey = std::array<std::byte, 32>;

// Is `key` admitted to `cluster` now? `noexcept`, thread-safe, non-blocking, std-only types in the signature.
class PinnedTrust {
public:
    virtual ~PinnedTrust() = default;
    [[nodiscard]] virtual bool admitted(ClusterId cluster, const Ed25519PublicKey& key) const noexcept = 0;
};

// The key the initiator PINS for a peer it dials: given the peer's (derived) NodeId within `cluster`, its full public
// key, or nullopt when the application knows none (the initiator then sends nothing at all). Same threading contract.
class PinnedKeyDirectory {
public:
    virtual ~PinnedKeyDirectory() = default;
    [[nodiscard]] virtual std::optional<Ed25519PublicKey> key_of(ClusterId cluster, NodeId peer) const noexcept = 0;
};

}  // namespace quark
