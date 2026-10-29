// Implements ADR-040 — the core, DELIBERATELY mbedTLS-ignorant DTOs for a node's TLS identity and
// its cluster trust roots; ADR-067 D3 — the identity CN grammar (member / client role). Parsing these DER blobs into `mbedtls_x509_crt`/`mbedtls_pk_context` happens
// only in the adapter (include/quark/adapters/mbedtls/); the core stays std-only per 020's "honest
// exception" (crypto is an opt-in adapter, never a core dependency) and CONVENTIONS.md's seam rule.
//
// `IdentityMaterial`/`TrustStore` are the two OverlappedMaterial<T> instantiations ADR-040 hot-reloads:
// a node's own certificate+key, and the cluster's trusted CA roots. See overlapped_material.hpp's
// header banner for how `previous` is used differently by each.
#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "quark/core/ids.hpp"
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
    std::vector<std::byte> private_key_der;  // DER: PKCS#8, or SEC1 for EC (the mbedTLS adapter parses both)
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

// ADR-067 D3: the two identity CN forms. A distinct client prefix (not a suffix) so a parser that only
// knows `quark:` refuses a client certificate instead of reading it as a member.
inline constexpr std::string_view kMemberCnPrefix = "quark:";
inline constexpr std::string_view kClientCnPrefix = "quark-client:";

namespace detail {
// Canonical unsigned decimal: non-empty, digits only, no leading zero (except "0"), fits in 64 bits,
// and consumed to the last byte. One identity has exactly one spelling.
[[nodiscard]] inline bool parse_canonical_u64(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty() || (s.size() > 1 && s.front() == '0')) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
}  // namespace detail

// Parse a certificate CN `quark:<cluster>:<node>` (member) or `quark-client:<cluster>:<node>` (client).
// The role must agree with the NodeId's client bit (kClientNodeBit): set for client, clear for member.
// Anything else — unknown prefix, missing or extra field, non-canonical number — is refused.
[[nodiscard]] inline bool parse_quark_cn(std::string_view cn, ClusterId& cluster, NodeId& node) noexcept {
    bool client = false;
    if (cn.starts_with(kClientCnPrefix)) {
        client = true;
        cn.remove_prefix(kClientCnPrefix.size());
    } else if (cn.starts_with(kMemberCnPrefix)) {
        cn.remove_prefix(kMemberCnPrefix.size());
    } else {
        return false;
    }
    const auto sep = cn.find(':');
    if (sep == std::string_view::npos) return false;
    std::uint64_t cluster_val = 0, node_val = 0;
    if (!detail::parse_canonical_u64(cn.substr(0, sep), cluster_val) ||
        !detail::parse_canonical_u64(cn.substr(sep + 1), node_val))
        return false;
    if (is_client_node(NodeId{node_val}) != client) return false;
    cluster = ClusterId{cluster_val};
    node = NodeId{node_val};
    return true;
}

// The one canonical CN for (cluster, node): the role prefix follows the NodeId's client bit. The inverse
// of parse_quark_cn — enrolment (ADR-067 D4) requires an issued certificate's CN to equal it byte for byte.
[[nodiscard]] inline std::string format_quark_cn(ClusterId cluster, NodeId node) {
    std::string cn(is_client_node(node) ? kClientCnPrefix : kMemberCnPrefix);
    cn += std::to_string(cluster.value);
    cn += ':';
    cn += std::to_string(node.value);
    return cn;
}

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
