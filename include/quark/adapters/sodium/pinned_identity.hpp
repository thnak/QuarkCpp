// Implements 020-Security §"Pinned-key mode (Noise IK, #40)" and ADR-068 — a node's pinned-mode identity: an Ed25519 key
// pair, the X25519 key Noise uses (converted from it), the NodeId DERIVED from the public key, and `PinnedKeyBook`, the
// ready-made allowlist implementing the application-facing `PinnedTrust` + `PinnedKeyDirectory` seams (core/pinned_key.hpp).
//
// KEY REUSE (documented, accepted): the Noise static key is the X25519 key converted from the Ed25519 identity key
// (libsodium crypto_sign_ed25519_pk_to_curve25519 / _sk_to_curve25519). One key therefore serves two roles — Ed25519
// signatures made by the application (e.g. its signed membership log) and X25519 DH inside the handshake. libsodium
// supports exactly this conversion; the published analyses of combined Ed25519/X25519 use (Thormarker 2021, and the Signal
// XEdDSA design) find no break when the two uses are domain-separated, and here they are: the handshake never signs, the
// application never DHes, and the Noise prologue binds a Quark-specific label. It is accepted because the alternative is a
// SECOND per-device key the application must also generate, pair, pin and rotate, and a pairing flow that pins one key
// is the consumer's stated requirement (#40). ADR-068 records it among the things an independent audit should look at.
//
// ONE-WAY MAP: Ed25519 -> X25519 is not invertible (the sign bit of x is lost), so a responder cannot recover the
// initiator's Ed25519 key from its Noise static. Message 1 therefore CARRIES the Ed25519 key in its (encrypted) payload and
// the responder checks that converting it yields exactly the Noise static that authenticated the message.
//
// LOCKED STORAGE (#64, AeroCoWorker ADR-047 D3): the Ed25519 secret and the derived X25519 static secret live in
// LockedBuffer — sodium_malloc'ed and mlock'ed with the result CHECKED — and an identity whose secret cannot be locked is
// REFUSED (from_secret_key returns nullopt). The handshake borrows these through NoiseStatic(); it never copies them.
// Everything transient is wiped on every exit path instead; see secure_memory.hpp for why only the identity is mlocked.
#pragma once

#include <sodium.h>

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>

#include "quark/adapters/sodium/noise.hpp"
#include "quark/adapters/sodium/secure_memory.hpp"
#include "quark/core/pinned_key.hpp"

namespace quark::adapters {

// How a NodeId is derived from a key (ADR-075). RoleBit is the only production value. LegacyRaw is ADR-068's original
// derivation (all 64 bits of the hash, so bit 63, ADR-074's client marker, is random); it exists ONLY as the control of the
// #63 checks, which show the defect it had. Never select it outside a test.
enum class NodeIdDerivation : std::uint8_t { RoleBit = 0, LegacyRaw = 1 };

// NodeId = (first 8 bytes, little-endian, of SHA-256 of the 32-byte Ed25519 public key, with bit 63 cleared) | role bit
// (set iff `role` is Client) -- ADR-075. Derived, never self-claimed: nobody can pick the low 63 bits without picking the
// key they hash to, and the application, not the hash, decides bit 63. NodeId 0 is "unspecified" in this API, so a key whose
// 63 hash bits are all zero (probability 2^-63) is refused (NodeId{0}) wherever a derived id is required.
[[nodiscard]] inline NodeId derive_pinned_node_id(const Ed25519PublicKey& key, PinnedRole role = PinnedRole::Member,
                                                  NodeIdDerivation how = NodeIdDerivation::RoleBit) noexcept {
    if (!noise::init()) return NodeId{0};
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    ::crypto_hash_sha256(digest.data(), noise::uc(key.data()), key.size());
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(digest[static_cast<std::size_t>(i)]) << (8 * i);
    if (how == NodeIdDerivation::LegacyRaw) return NodeId{v};
    const std::uint64_t low = v & ~kClientNodeBit;
    if (low == 0) return NodeId{0};
    return NodeId{low | (role == PinnedRole::Client ? kClientNodeBit : 0)};
}

// The 63 key-derived bits of a NodeId (the whole value for LegacyRaw): what two keys must not share, whatever their roles.
[[nodiscard]] constexpr std::uint64_t pinned_key_bits(NodeId n, NodeIdDerivation how = NodeIdDerivation::RoleBit) noexcept {
    return how == NodeIdDerivation::LegacyRaw ? n.value : (n.value & ~kClientNodeBit);
}

class PinnedIdentity {
public:
    static constexpr std::size_t kSecretKeyBytes = crypto_sign_ed25519_SECRETKEYBYTES;  // 64: seed || public key

    // A BORROWED view of the node's Noise static key (#64): `sk` points into this identity's LOCKED buffer and is valid
    // for the identity's lifetime; `pk` is public. The handshake takes this view instead of copying the secret.
    struct NoiseStatic {
        std::span<const std::byte> sk;
        noise::Key32 pk{};
    };

    // From a libsodium Ed25519 secret key (seed || public key), e.g. one the application persisted. nullopt when the
    // key is inconsistent, its public half cannot be converted to X25519, or the secret cannot be held in LOCKED memory:
    // fail closed (#64, AeroCoWorker ADR-047 D3) — an unlocked copy of the seed would defeat the application's custody.
    // `role` is the role the APPLICATION assigns this device (ADR-075); it sets bit 63 of the NodeId and must equal the role
    // every peer's trust data holds for this key, or peers refuse the handshake.
    [[nodiscard]] static std::optional<PinnedIdentity> from_secret_key(std::span<const std::byte, kSecretKeyBytes> sk,
                                                                       PinnedRole role = PinnedRole::Member,
                                                                       NodeIdDerivation how = NodeIdDerivation::RoleBit) {
        if (!noise::init()) return std::nullopt;
        PinnedIdentity id;
        id.secret_ = LockedBuffer(kSecretKeyBytes);
        id.x_sk_ = LockedBuffer(noise::kKeyLen);
        if (!id.secret_.ok() || !id.x_sk_.ok()) return std::nullopt;  // no lock => no identity
        std::memcpy(id.secret_.data(), sk.data(), kSecretKeyBytes);
        if (::crypto_sign_ed25519_sk_to_pk(noise::uc(id.public_.data()), noise::uc(id.secret_.data())) != 0)
            return std::nullopt;
        if (::crypto_sign_ed25519_sk_to_curve25519(noise::uc(id.x_sk_.data()), noise::uc(id.secret_.data())) != 0)
            return std::nullopt;
        if (::crypto_sign_ed25519_pk_to_curve25519(noise::uc(id.x_pk_.data()), noise::uc(id.public_.data())) != 0)
            return std::nullopt;
        // The two derivations of the X25519 public key must agree; if they ever did not, fail closed.
        if (noise::public_from_private(id.x_sk_.span()) != id.x_pk_) return std::nullopt;
        id.role_ = role;
        id.node_ = derive_pinned_node_id(id.public_, role, how);
        if (id.node_.value == 0) return std::nullopt;
        return id;
    }

    // A fresh random identity (provisioning and tests). Throws if libsodium is unavailable or a key cannot be locked:
    // the retry exists only for the 2^-63 NodeId-0 case and is BOUNDED, so a refused lock cannot spin here forever.
    [[nodiscard]] static PinnedIdentity generate(PinnedRole role = PinnedRole::Member,
                                                 NodeIdDerivation how = NodeIdDerivation::RoleBit) {
        if (!noise::init()) throw std::runtime_error("PinnedIdentity: libsodium unavailable");
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::array<std::byte, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
            std::array<std::byte, kSecretKeyBytes> sk{};
            ::crypto_sign_ed25519_keypair(noise::uc(pk.data()), noise::uc(sk.data()));
            auto id = from_secret_key(std::span<const std::byte, kSecretKeyBytes>(sk), role, how);
            noise::wipe(sk);
            if (id) return std::move(*id);
        }
        throw std::runtime_error("PinnedIdentity: cannot lock a fresh key (sodium_malloc/mlock refused)");
    }

    PinnedIdentity(const PinnedIdentity&) = delete;
    PinnedIdentity& operator=(const PinnedIdentity&) = delete;
    // Moving transfers the locked buffers; the source owns nothing afterwards, so nothing is left unwiped.
    PinnedIdentity(PinnedIdentity&&) noexcept = default;
    PinnedIdentity& operator=(PinnedIdentity&&) noexcept = default;
    ~PinnedIdentity() = default;  // LockedBuffer zeroes, unlocks and frees

    [[nodiscard]] const Ed25519PublicKey& public_key() const noexcept { return public_; }
    [[nodiscard]] NodeId node_id() const noexcept { return node_; }
    [[nodiscard]] PinnedRole role() const noexcept { return role_; }
    // For the application to persist (it owns key storage; Quark never writes it anywhere).
    [[nodiscard]] std::span<const std::byte, kSecretKeyBytes> secret_key() const noexcept {
        return std::span<const std::byte, kSecretKeyBytes>(secret_.data(), kSecretKeyBytes);
    }
    [[nodiscard]] NoiseStatic noise_static() const noexcept { return NoiseStatic{x_sk_.span(), x_pk_}; }

private:
    PinnedIdentity() = default;
    Ed25519PublicKey public_{};
    NodeId node_{};
    PinnedRole role_ = PinnedRole::Member;
    LockedBuffer secret_;   // 64 B, LOCKED: the Ed25519 seed || public key
    LockedBuffer x_sk_;     // 32 B, LOCKED: the derived X25519 static secret
    noise::Key32 x_pk_{};   // public half
};

// Ed25519 -> X25519 for a PEER's public key. False for an invalid or small-order point.
[[nodiscard]] inline bool ed25519_to_x25519(const Ed25519PublicKey& ed, noise::Key32& x) noexcept {
    return noise::init() && ::crypto_sign_ed25519_pk_to_curve25519(noise::uc(x.data()), noise::uc(ed.data())) == 0;
}

// A ready-made trust provider + key directory: an allowlist of full Ed25519 keys per cluster, mutable at run time (an
// application whose membership log changes calls admit()/revoke()). Consulted fresh at every handshake, so a change is
// seen by the NEXT handshake; live sessions are not touched (#41). Thread-safe (reader/writer lock).
class PinnedKeyBook final : public PinnedTrust, public PinnedKeyDirectory {
public:
    explicit PinnedKeyBook(NodeIdDerivation how = NodeIdDerivation::RoleBit) : how_(how) {}

    // Admit `key` to `cluster` in `role` (ADR-075; default Member). False (and nothing changes) if:
    //  - a DIFFERENT key already holds the same 63 key-derived NodeId bits in that cluster (whatever role either has): the
    //    NodeId is only 63 bits of a hash, so the application, which chooses what it admits, refuses a colliding key rather
    //    than let two admitted keys share a routing identity;
    //  - the key is ALREADY admitted in the OTHER role. ROLE CONFLICT POLICY: refuse, never replace. A key is admitted in one
    //    role per cluster; the application changes it deliberately with revoke() then admit(), so a role flip is never a side
    //    effect of an admit call (and a stale or hostile "admit as Member" cannot demote a Client in place).
    // Re-admitting a key in the role it already has is a successful no-op.
    bool admit(ClusterId cluster, const Ed25519PublicKey& key, PinnedRole role = PinnedRole::Member) {
        const NodeId node = derive_pinned_node_id(key, role, how_);
        if (node.value == 0) return false;
        // A and -A (sign bit of the last byte flipped) convert to the SAME X25519 key, so whoever holds A's secret can
        // authenticate as -A, which has a different NodeId. Trust compares exact Ed25519 bytes, so that is only safe if
        // the book never admits two keys with one X25519 image: refuse the second.
        noise::Key32 image{};
        if (!ed25519_to_x25519(key, image)) return false;
        std::unique_lock g(mu_);
        auto& m = by_cluster_[cluster.value];
        for (const auto& [n, other] : m) {
            noise::Key32 other_image{};
            if (!(other.key == key) && ed25519_to_x25519(other.key, other_image) && other_image == image) return false;
        }
        const std::uint64_t slot = pinned_key_bits(node, how_);
        const auto it = m.find(slot);
        if (it != m.end()) return it->second.key == key && it->second.role == role;  // same key+role: ok; else refuse
        m.emplace(slot, Entry{key, role});
        return true;
    }
    bool revoke(ClusterId cluster, const Ed25519PublicKey& key) {
        const std::uint64_t slot = pinned_key_bits(derive_pinned_node_id(key, PinnedRole::Member, how_), how_);
        std::unique_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return false;
        const auto it = c->second.find(slot);
        if (it == c->second.end() || !(it->second.key == key)) return false;
        c->second.erase(it);
        return true;
    }

    [[nodiscard]] bool admitted(ClusterId cluster, const Ed25519PublicKey& key) const noexcept override {
        return role_of(cluster, key).has_value();
    }

    [[nodiscard]] std::optional<PinnedRole> role_of(ClusterId cluster, const Ed25519PublicKey& key) const noexcept override {
        const std::uint64_t slot = pinned_key_bits(derive_pinned_node_id(key, PinnedRole::Member, how_), how_);
        std::shared_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return std::nullopt;
        const auto it = c->second.find(slot);
        if (it == c->second.end() || !(it->second.key == key)) return std::nullopt;  // the FULL key must match
        return it->second.role;
    }

    // `peer` carries the role bit: a peer id whose bit 63 disagrees with the role this key was admitted in finds nothing.
    [[nodiscard]] std::optional<Ed25519PublicKey> key_of(ClusterId cluster, NodeId peer) const noexcept override {
        std::shared_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return std::nullopt;
        const auto it = c->second.find(pinned_key_bits(peer, how_));
        if (it == c->second.end()) return std::nullopt;
        if (how_ == NodeIdDerivation::RoleBit && is_client_node(peer) != (it->second.role == PinnedRole::Client))
            return std::nullopt;
        return it->second.key;
    }

private:
    struct Entry {
        Ed25519PublicKey key;
        PinnedRole role;
    };
    NodeIdDerivation how_;
    mutable std::shared_mutex mu_;
    // cluster -> (63 key-derived NodeId bits -> entry). Keyed WITHOUT the role bit so two keys that collide in the 63 bits
    // are refused whatever role each holds.
    std::map<std::uint64_t, std::map<std::uint64_t, Entry>> by_cluster_;
};

}  // namespace quark::adapters
