// Implements ADR-067 D1/D2 — the issuer-authenticated revocation RECORD and the verify-only trust-anchor
// seam (020 §"Rotation and revocation invariants"). Std-only: the signature check itself is an adapter
// (include/quark/adapters/mbedtls/mbedtls_trust_anchor.hpp); the core never holds a signing key and
// contains no signing primitive (ADR-067 C5).
//
// A record is signed by the cluster trust anchor (the CA root key that issues node certificates) over a
// FIXED-LENGTH, fixed-layout message whose first 16 bytes are an ASCII domain-separation prefix — it can
// never begin a DER SEQUENCE (0x30), so a revocation signature is never a valid certificate signature and
// vice versa (ADR-067 C2). Records are identified by `fingerprint`, never by their bytes: ECDSA is
// malleable, so two valid encodings of one revocation are one revocation. Replay is harmless (the
// registry is a union-only ratchet), so the record has no nonce and `issued_at` is never a validity gate.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "quark/core/ids.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark {

// SHA-256 of the signing root's SubjectPublicKeyInfo DER — selects the root (current or still-in-window
// previous) a record must verify under.
using AnchorKeyId = std::array<std::byte, 32>;

inline constexpr std::string_view kRevocationDomain{"QUARK-REVOKE-v1\0", 16};
inline constexpr std::size_t kRevocationSignedBytes = 16 + 8 + 32 + 8 + 8 + 32;  // = 104
inline constexpr std::size_t kMaxRevocationSignatureBytes = 72;                  // DER ECDSA P-256
inline constexpr std::size_t kMaxSignedRevocationWireBytes =
    kRevocationSignedBytes + 1 + kMaxRevocationSignatureBytes;

struct SignedRevocation {
    ClusterId cluster{};
    Fingerprint fingerprint{};          // SHA-256 of the revoked leaf (RevocationRegistry's key)
    std::uint64_t not_after = 0;        // the revoked leaf's notAfter, unix seconds — the prune horizon
    std::uint64_t issued_at = 0;        // unix seconds; audit only
    AnchorKeyId anchor_key_id{};
    std::vector<std::byte> signature;   // DER ECDSA-with-SHA-256 over revocation_signed_bytes()
};

namespace detail {
inline void put_le64(std::byte* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}
[[nodiscard]] inline std::uint64_t get_le64(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
}  // namespace detail

// The exact bytes the trust anchor signs: prefix ‖ cluster ‖ fingerprint ‖ not_after ‖ issued_at ‖
// anchor_key_id (integers little-endian).
[[nodiscard]] inline std::array<std::byte, kRevocationSignedBytes> revocation_signed_bytes(
    const SignedRevocation& r) noexcept {
    std::array<std::byte, kRevocationSignedBytes> out{};
    std::byte* p = out.data();
    for (char ch : kRevocationDomain) *p++ = static_cast<std::byte>(ch);
    detail::put_le64(p, r.cluster.value);
    p += 8;
    for (std::byte b : r.fingerprint) *p++ = b;
    detail::put_le64(p, r.not_after);
    p += 8;
    detail::put_le64(p, r.issued_at);
    p += 8;
    for (std::byte b : r.anchor_key_id) *p++ = b;
    return out;
}

// Wire form (gossip, persistence): signed bytes ‖ u8 signature length ‖ signature.
[[nodiscard]] inline std::vector<std::byte> encode_signed_revocation(const SignedRevocation& r) {
    const auto signed_bytes = revocation_signed_bytes(r);
    std::vector<std::byte> out(signed_bytes.begin(), signed_bytes.end());
    out.push_back(static_cast<std::byte>(r.signature.size()));
    out.insert(out.end(), r.signature.begin(), r.signature.end());
    return out;
}

// Exact-length decode: the domain prefix must match, the signature length must be 1..72 and must account
// for every remaining byte. Anything else is refused (`out` is then unspecified).
[[nodiscard]] inline bool decode_signed_revocation(std::span<const std::byte> in, SignedRevocation& out) {
    if (in.size() < kRevocationSignedBytes + 1 || in.size() > kMaxSignedRevocationWireBytes) return false;
    const std::byte* p = in.data();
    for (char ch : kRevocationDomain)
        if (*p++ != static_cast<std::byte>(ch)) return false;
    out.cluster = ClusterId{detail::get_le64(p)};
    p += 8;
    for (std::byte& b : out.fingerprint) b = *p++;
    out.not_after = detail::get_le64(p);
    p += 8;
    out.issued_at = detail::get_le64(p);
    p += 8;
    for (std::byte& b : out.anchor_key_id) b = *p++;
    const std::size_t sig_len = static_cast<unsigned char>(*p++);
    if (sig_len == 0 || sig_len > kMaxRevocationSignatureBytes || in.size() != kRevocationSignedBytes + 1 + sig_len)
        return false;
    out.signature.assign(p, p + sig_len);
    return true;
}

// The verify-only trust-anchor seam (ADR-067 D1). `verify` is true iff `anchor_key_id` names a CURRENTLY
// trusted root (current, or previous still inside its ADR-040 overlap window) AND `signature` is a valid
// ECDSA-with-SHA-256 signature by that root over `message`. Implementations must be safe to call from any
// thread; it is a cold path (one call per never-seen fingerprint).
class TrustAnchorVerifier {
public:
    virtual ~TrustAnchorVerifier() = default;
    [[nodiscard]] virtual bool verify(const AnchorKeyId& anchor_key_id, std::span<const std::byte> message,
                                      std::span<const std::byte> signature) const = 0;
    // True iff `anchor_key_id` names a root in the CURRENT trust set (not merely the overlap-window
    // previous one). During a root rotation this is what lets a record re-signed under the new root
    // replace the old-root copy — and never the reverse, so the two copies cannot flip-flop.
    [[nodiscard]] virtual bool is_current_root(const AnchorKeyId& anchor_key_id) const = 0;
};

// Full record check against a verifier and the local cluster id.
[[nodiscard]] inline bool verify_signed_revocation(const SignedRevocation& r, ClusterId cluster,
                                                   const TrustAnchorVerifier& verifier) {
    if (r.cluster != cluster || r.signature.empty() || r.signature.size() > kMaxRevocationSignatureBytes)
        return false;
    const auto message = revocation_signed_bytes(r);
    return verifier.verify(r.anchor_key_id, message, r.signature);
}

}  // namespace quark
