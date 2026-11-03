// Implements ADR-040 / ADR-074 D1/D4 — shared X.509 helpers for the mbedTLS adapters, so the TLS handshake
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

#include "quark/core/error.hpp"        // result / errc / fail (#39 startup validation)
#include "quark/core/tls_identity.hpp"  // TlsIdentity / TrustStore

namespace quark::adapters::detail {

// Owning wrappers so every early return frees what was parsed.
struct Crt {
    mbedtls_x509_crt v;
    Crt() { mbedtls_x509_crt_init(&v); }
    ~Crt() { mbedtls_x509_crt_free(&v); }
    Crt(const Crt&) = delete;
    Crt& operator=(const Crt&) = delete;
};
struct Pk {
    mbedtls_pk_context v;
    Pk() { mbedtls_pk_init(&v); }
    ~Pk() { mbedtls_pk_free(&v); }
    Pk(const Pk&) = delete;
    Pk& operator=(const Pk&) = delete;
};

[[nodiscard]] inline const unsigned char* uchars(std::span<const std::byte> s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

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

// The value of the Subject's commonName, if there is EXACTLY ONE (ADR-074 D3). mbedtls_x509_name is a
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

// ADR-074 D1/C5: a certificate that can act as a CA — basicConstraints CA:TRUE, or keyUsage keyCertSign or
// cRLSign. Neither a node identity nor an issued enrolment certificate may be one. keyUsage absent means
// unrestricted, but CA:TRUE is then still required to sign certificates, so absent keyUsage is fine here.
[[nodiscard]] inline bool ca_capable(const mbedtls_x509_crt& crt) {
    if (mbedtls_x509_crt_get_ca_istrue(&crt) == 1) return true;
    return mbedtls_x509_crt_has_ext_type(&crt, MBEDTLS_X509_EXT_KEY_USAGE) &&
           (mbedtls_x509_crt_check_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN) == 0 ||
            mbedtls_x509_crt_check_key_usage(&crt, MBEDTLS_X509_KU_CRL_SIGN) == 0);
}

// ADR-074 C5: true if `key`'s public key equals the public key of any certificate in `roots` — i.e. the
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

namespace quark::adapters {

// ADR-074 C5 at startup: refuse a node identity that is CA key material — its private key or certificate
// key equal to the key of any root in `trust` (current and still-in-window previous), or a CA-capable
// certificate (an issuing intermediate's). Lives here (rather than in mbedtls_enrollment.hpp) so the
// handshake seam can apply it before the transport starts; MbedtlsHandshakeEngine enforces the same rule
// on every handshake. The key is parsed without an RNG: a node identity's DER is unencrypted.
[[nodiscard]] inline result<void> validate_identity_not_root(const TlsIdentity& own,
                                                             const TrustStore::Snapshot& trust) {
    detail::Pk key;
    detail::Crt cert, roots;
    if (mbedtls_pk_parse_key(&key.v, detail::uchars(own.private_key_der), own.private_key_der.size(),
                             nullptr, 0, nullptr, nullptr) != 0)
        return fail(errc::validation, "own identity private key does not parse (ADR-074 C5)");
    if (mbedtls_x509_crt_parse_der(&cert.v, detail::uchars(own.cert_chain_der), own.cert_chain_der.size()) != 0)
        return fail(errc::validation, "own identity certificate does not parse (ADR-074 C5)");
    for (const TrustedRoots* bundle : {trust.current.get(), trust.previous.get()}) {
        if (bundle == nullptr || bundle->ca_chain_der.empty()) continue;
        if (!detail::parse_der_concat(roots.v, bundle->ca_chain_der))
            return fail(errc::validation, "trust roots do not parse (ADR-074 C5)");
    }
    if (roots.v.raw.len == 0) return fail(errc::validation, "no trust roots configured (ADR-074 C5)");
    if (detail::key_is_a_root(key.v, roots.v) || detail::key_is_a_root(cert.v.pk, roots.v) ||
        detail::ca_capable(cert.v))
        return fail(errc::validation, "own identity is CA key material: a CA key must never be in a Quark process "
                                      "(ADR-074 C5)");
    return {};
}

}  // namespace quark::adapters
