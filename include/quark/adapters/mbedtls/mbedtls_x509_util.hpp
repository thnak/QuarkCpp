// Implements ADR-040 / ADR-067 D1/D4 — shared X.509 helpers for the mbedTLS adapters, so the TLS handshake
// (mbedtls_handshake.hpp), the revocation verifier (mbedtls_trust_anchor.hpp) and the enrolment checks
// (mbedtls_enrollment.hpp) parse a trust bundle and a Subject the same way, and therefore trust exactly
// the same roots and read exactly the same identity.
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

namespace quark::adapters::detail {

// Append every certificate of a concatenation of DER certificates (each self-delimiting) to `chain`.
// `mbedtls_x509_crt_parse_der` parses ONE certificate per call, so a multi-root bundle must be walked.
// Fails closed: false on any certificate that does not parse, or on empty input.
[[nodiscard]] inline bool parse_der_concat(mbedtls_x509_crt& chain, std::span<const std::byte> der) {
    if (der.empty()) return false;
    std::size_t off = 0;
    while (off < der.size()) {
        if (mbedtls_x509_crt_parse_der(&chain, reinterpret_cast<const unsigned char*>(der.data() + off),
                                       der.size() - off) != 0)
            return false;
        const mbedtls_x509_crt* last = &chain;
        while (last->next != nullptr) last = last->next;
        if (last->raw.len == 0) return false;
        off += last->raw.len;
    }
    return true;
}

// The value of the Subject's commonName, if there is EXACTLY ONE (ADR-067 D3). mbedtls_x509_name is a
// linked list of {oid, val} pairs (asn1.h); every attribute is walked so a second CN is seen wherever it
// sits. False when there is none or more than one.
[[nodiscard]] inline bool single_cn(const mbedtls_x509_crt& crt, std::string_view& cn) {
    const std::size_t cn_oid_len = std::strlen(MBEDTLS_OID_AT_CN);
    const mbedtls_x509_name* cn_attr = nullptr;
    for (const mbedtls_x509_name* n = &crt.subject; n != nullptr; n = n->next) {
        if (n->oid.p == nullptr || n->oid.len != cn_oid_len ||
            std::memcmp(n->oid.p, MBEDTLS_OID_AT_CN, cn_oid_len) != 0)
            continue;
        if (cn_attr != nullptr) return false;  // a second CN: ambiguous identity
        cn_attr = n;
    }
    if (cn_attr == nullptr) return false;
    cn = std::string_view(reinterpret_cast<const char*>(cn_attr->val.p), cn_attr->val.len);
    return true;
}

// The DER SubjectPublicKeyInfo of `pk`. mbedtls_pk_write_pubkey_der writes at the END of the buffer;
// the result is that tail. Empty on failure.
inline constexpr std::size_t kMaxSpkiBytes = 2048;  // fits every key mbedTLS writes (an RSA-8192 SPKI is ~1.1 KB)
struct Spki {
    std::array<unsigned char, kMaxSpkiBytes> buf{};
    std::size_t len = 0;
    [[nodiscard]] std::span<const unsigned char> bytes() const { return {buf.data() + buf.size() - len, len}; }
};
[[nodiscard]] inline bool spki_of(const mbedtls_pk_context& pk, Spki& out) {
    const int len = mbedtls_pk_write_pubkey_der(&pk, out.buf.data(), out.buf.size());
    if (len <= 0) return false;
    out.len = static_cast<std::size_t>(len);
    return true;
}

[[nodiscard]] inline bool same_bytes(std::span<const unsigned char> a, std::span<const unsigned char> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// ADR-067 D1/C5: a certificate that can act as a CA — basicConstraints CA:TRUE, or keyUsage keyCertSign or
// cRLSign. Neither a node identity nor an issued enrolment certificate may be one. keyUsage absent means
// unrestricted, but CA:TRUE is then still required to sign certificates, so absent keyUsage is fine here.
[[nodiscard]] inline bool ca_capable(const mbedtls_x509_crt& crt) {
    if (mbedtls_x509_crt_get_ca_istrue(&crt) == 1) return true;
    return mbedtls_x509_crt_has_ext_type(&crt, MBEDTLS_X509_EXT_KEY_USAGE) &&
           (mbedtls_x509_crt_check_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN) == 0 ||
            mbedtls_x509_crt_check_key_usage(&crt, MBEDTLS_X509_KU_CRL_SIGN) == 0);
}

// ADR-067 C5: true if `key`'s public key equals the public key of any certificate in `roots` — i.e. the
// node would be running with a trusted root's private key in process. Fails closed: a key whose SPKI
// cannot be written counts as a match.
[[nodiscard]] inline bool key_is_a_root(const mbedtls_pk_context& key, const mbedtls_x509_crt& roots) {
    Spki own;
    if (!spki_of(key, own)) return true;
    for (const mbedtls_x509_crt* c = &roots; c != nullptr && c->raw.len != 0; c = c->next) {
        Spki root;
        if (spki_of(c->pk, root) && same_bytes(own.bytes(), root.bytes())) return true;
    }
    return false;
}

}  // namespace quark::adapters::detail
