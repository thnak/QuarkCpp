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
#include "quark/core/pinned_key.hpp"

namespace quark::adapters {

// NodeId = first 8 bytes (little-endian) of SHA-256 of the 32-byte Ed25519 public key. Derived, never self-claimed:
// nobody can pick a NodeId without picking the key it hashes to. NodeId 0 is "unspecified" in this API, so a key that
// hashes to 0 (probability 2^-64) is simply refused wherever a derived id is required.
[[nodiscard]] inline NodeId derive_pinned_node_id(const Ed25519PublicKey& key) noexcept {
    if (!noise::init()) return NodeId{0};
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    ::crypto_hash_sha256(digest.data(), noise::uc(key.data()), key.size());
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(digest[static_cast<std::size_t>(i)]) << (8 * i);
    return NodeId{v};
}

class PinnedIdentity {
public:
    static constexpr std::size_t kSecretKeyBytes = crypto_sign_ed25519_SECRETKEYBYTES;  // 64: seed || public key

    // From a libsodium Ed25519 secret key (seed || public key), e.g. one the application persisted. nullopt when the
    // key is inconsistent or its public half cannot be converted to X25519.
    [[nodiscard]] static std::optional<PinnedIdentity> from_secret_key(std::span<const std::byte, kSecretKeyBytes> sk) {
        if (!noise::init()) return std::nullopt;
        PinnedIdentity id;
        std::memcpy(id.secret_.data(), sk.data(), kSecretKeyBytes);
        if (::crypto_sign_ed25519_sk_to_pk(noise::uc(id.public_.data()), noise::uc(id.secret_.data())) != 0)
            return std::nullopt;
        if (::crypto_sign_ed25519_sk_to_curve25519(noise::uc(id.noise_.sk.data()), noise::uc(id.secret_.data())) != 0)
            return std::nullopt;
        if (::crypto_sign_ed25519_pk_to_curve25519(noise::uc(id.noise_.pk.data()), noise::uc(id.public_.data())) != 0)
            return std::nullopt;
        // The two derivations of the X25519 public key must agree; if they ever did not, fail closed.
        if (noise::public_from_private(id.noise_.sk) != id.noise_.pk) return std::nullopt;
        id.node_ = derive_pinned_node_id(id.public_);
        if (id.node_.value == 0) return std::nullopt;
        return id;
    }

    // A fresh random identity (provisioning and tests). Throws only if libsodium cannot initialise.
    [[nodiscard]] static PinnedIdentity generate() {
        if (!noise::init()) throw std::runtime_error("PinnedIdentity: libsodium unavailable");
        for (;;) {
            std::array<std::byte, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
            std::array<std::byte, kSecretKeyBytes> sk{};
            ::crypto_sign_ed25519_keypair(noise::uc(pk.data()), noise::uc(sk.data()));
            auto id = from_secret_key(std::span<const std::byte, kSecretKeyBytes>(sk));
            noise::wipe(sk);
            if (id) return std::move(*id);
        }
    }

    PinnedIdentity(const PinnedIdentity&) = delete;
    PinnedIdentity& operator=(const PinnedIdentity&) = delete;
    PinnedIdentity(PinnedIdentity&& o) noexcept
        : public_(o.public_), node_(o.node_), secret_(o.secret_), noise_(o.noise_) {
        o.wipe_secrets();
    }
    PinnedIdentity& operator=(PinnedIdentity&& o) noexcept {
        if (this != &o) {
            wipe_secrets();
            public_ = o.public_;
            node_ = o.node_;
            secret_ = o.secret_;
            noise_ = o.noise_;
            o.wipe_secrets();
        }
        return *this;
    }
    ~PinnedIdentity() { wipe_secrets(); }

    [[nodiscard]] const Ed25519PublicKey& public_key() const noexcept { return public_; }
    [[nodiscard]] NodeId node_id() const noexcept { return node_; }
    // For the application to persist (it owns key storage; Quark never writes it anywhere).
    [[nodiscard]] std::span<const std::byte, kSecretKeyBytes> secret_key() const noexcept {
        return std::span<const std::byte, kSecretKeyBytes>(secret_);
    }
    [[nodiscard]] const noise::KeyPair& noise_static() const noexcept { return noise_; }

private:
    PinnedIdentity() = default;
    void wipe_secrets() noexcept {
        noise::wipe(secret_);
        noise::wipe(noise_.sk);
    }
    Ed25519PublicKey public_{};
    NodeId node_{};
    std::array<std::byte, kSecretKeyBytes> secret_{};
    noise::KeyPair noise_{};
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
    // Admit `key` to `cluster`. False (and nothing changes) if a DIFFERENT key already holds the same derived NodeId in
    // that cluster: the NodeId is 64 bits of a hash, so the application, which chooses what it admits, refuses a
    // colliding key rather than let two admitted keys share a routing identity.
    bool admit(ClusterId cluster, const Ed25519PublicKey& key) {
        const NodeId node = derive_pinned_node_id(key);
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
            if (!(other == key) && ed25519_to_x25519(other, other_image) && other_image == image) return false;
        }
        const auto it = m.find(node.value);
        if (it != m.end()) return it->second == key;  // already admitted: ok; a different key: refuse
        m.emplace(node.value, key);
        return true;
    }
    bool revoke(ClusterId cluster, const Ed25519PublicKey& key) {
        const NodeId node = derive_pinned_node_id(key);
        std::unique_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return false;
        const auto it = c->second.find(node.value);
        if (it == c->second.end() || !(it->second == key)) return false;
        c->second.erase(it);
        return true;
    }

    [[nodiscard]] bool admitted(ClusterId cluster, const Ed25519PublicKey& key) const noexcept override {
        const NodeId node = derive_pinned_node_id(key);
        std::shared_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return false;
        const auto it = c->second.find(node.value);
        return it != c->second.end() && it->second == key;  // the FULL key must match, not just its hash prefix
    }

    [[nodiscard]] std::optional<Ed25519PublicKey> key_of(ClusterId cluster, NodeId peer) const noexcept override {
        std::shared_lock g(mu_);
        const auto c = by_cluster_.find(cluster.value);
        if (c == by_cluster_.end()) return std::nullopt;
        const auto it = c->second.find(peer.value);
        if (it == c->second.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::shared_mutex mu_;
    std::map<std::uint64_t, std::map<std::uint64_t, Ed25519PublicKey>> by_cluster_;
};

}  // namespace quark::adapters
