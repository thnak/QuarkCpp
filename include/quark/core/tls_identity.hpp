// Implements ADR-040 — the core, DELIBERATELY mbedTLS-ignorant DTOs for a node's TLS identity and
// its cluster trust roots. Parsing these DER blobs into `mbedtls_x509_crt`/`mbedtls_pk_context` happens
// only in the adapter (include/quark/adapters/mbedtls/); the core stays std-only per 020's "honest
// exception" (crypto is an opt-in adapter, never a core dependency) and CONVENTIONS.md's seam rule.
//
// `IdentityMaterial`/`TrustStore` are the two OverlappedMaterial<T> instantiations ADR-040 hot-reloads:
// a node's own certificate+key, and the cluster's trusted CA roots. See overlapped_material.hpp's
// header banner for how `previous` is used differently by each.
#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "quark/core/overlapped_material.hpp"

namespace quark {

// SHA-256 of a leaf certificate's DER encoding. Shared identity for PeerSession (the session's
// authenticated peer) and RevocationRegistry (the compromised-key blocklist) — precomputed once at
// identity-load time so the hot revocation-compare path (SecureTransport::sweep_revocations) never
// rehashes a certificate.
using Fingerprint = std::array<std::byte, 32>;

// This node's own certificate chain + private key, as loaded from operator-provided PEM/DER (the
// concrete provisioning source is a 021 seam concern, not this type's). `fingerprint` is the SHA-256
// of `cert_chain_der`'s leaf entry, precomputed at load.
struct TlsIdentity {
    std::vector<std::byte> cert_chain_der;   // leaf cert (+ any intermediates), concatenated DER
    std::vector<std::byte> private_key_der;  // PKCS8 DER
    Fingerprint fingerprint{};
};

// The cluster's trusted CA root(s), concatenated DER. During the S4 overlap window a peer's chain is
// verified against the UNION of current + previous (see overlapped_material.hpp banner) — built fresh
// per handshake attempt, a cold-path cost.
struct TrustedRoots {
    std::vector<std::byte> ca_chain_der;
};

using IdentityMaterial = OverlappedMaterial<TlsIdentity>;
using TrustStore = OverlappedMaterial<TrustedRoots>;

}  // namespace quark

template <>
struct std::hash<quark::Fingerprint> {
    std::size_t operator()(const quark::Fingerprint& fp) const noexcept {
        // FNV-1a over the 32 raw bytes — cold/compare-path only (handshake, revocation sweep), never
        // the message hot path, so a simple byte-fold is fine (no need for splitmix64's avalanche).
        std::size_t h = 1469598103934665603ULL;
        for (std::byte b : fp) {
            h ^= static_cast<unsigned char>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }
};
