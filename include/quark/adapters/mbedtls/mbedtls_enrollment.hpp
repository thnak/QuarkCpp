// Implements ADR-067 D4 (and C5) — the mbedTLS side of ticketed enrolment (021 §Enrolment):
//   * `MbedtlsEnrolCrypto`, the anchor's implementation of the verify-only `enrol::EnrolCrypto` seam
//     (enrollment.hpp): constant-time HMAC-SHA256, CSR proof-of-possession, and the checks on the
//     certificate the application's custodian returns;
//   * the joiner's half: generating its identity key, the CSR, and the ticket MAC;
//   * `validate_identity_not_root`, the C5 startup check (a node never runs with a root's private key).
//
// NOTHING HERE SIGNS A CERTIFICATE (ADR-067 D1). The only private key this file touches is the joiner's
// own identity key, which signs its own CSR. Every certificate check is against the anchor's CONFIGURED
// TrustStore (current roots plus the still-in-window previous ones, ADR-040), parsed with the same walker
// the handshake uses (mbedtls_x509_util.hpp).
//
// Keys are ECDSA P-256 only, the curve the cluster's roots use (ADR-067 D1): a CSR for any other key is
// refused. Cold path; every call parses what it needs and keeps no shared mbedTLS state (the only shared
// object is the TrustStore, whose snapshot() is itself thread-safe), so `MbedtlsEnrolCrypto` is thread-safe.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <string_view>
#include <vector>

#include <mbedtls/asn1.h>
// mbedTLS 3.6's constant_time.h, unlike its other public headers, has no extern "C" guard of its own.
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include "quark/adapters/mbedtls/mbedtls_handshake.hpp"  // detail::SharedRng
#include "quark/adapters/mbedtls/mbedtls_x509_util.hpp"
#include "quark/core/enrollment.hpp"
#include "quark/core/error.hpp"
#include "quark/core/secret.hpp"
#include "quark/core/tls_identity.hpp"

namespace quark::adapters {

namespace detail {

inline constexpr std::size_t kMaxCsrBytes = 4096;
inline constexpr std::size_t kMaxIssuedChainBytes = 16384;
inline constexpr std::size_t kMaxKeyDerBytes = 512;

// Owning wrappers so every early return frees what was parsed.
struct Crt {
    mbedtls_x509_crt v;
    Crt() { mbedtls_x509_crt_init(&v); }
    ~Crt() { mbedtls_x509_crt_free(&v); }
    Crt(const Crt&) = delete;
    Crt& operator=(const Crt&) = delete;
};
struct Csr {
    mbedtls_x509_csr v;
    Csr() { mbedtls_x509_csr_init(&v); }
    ~Csr() { mbedtls_x509_csr_free(&v); }
    Csr(const Csr&) = delete;
    Csr& operator=(const Csr&) = delete;
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

[[nodiscard]] inline bool is_p256(const mbedtls_pk_context& pk) {
    return mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY &&
           mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(pk)) == MBEDTLS_ECP_DP_SECP256R1;
}

[[nodiscard]] inline bool parse_private_key(mbedtls_pk_context& pk, std::span<const std::byte> key_der) {
    return mbedtls_pk_parse_key(&pk, uchars(key_der), key_der.size(), nullptr, 0, &SharedRng::random,
                                &SharedRng::instance()) == 0;
}

// Seconds since the Unix epoch of an X.509 time (UTC, proleptic Gregorian; days_from_civil after
// H. Hinnant). False for an out-of-range field.
[[nodiscard]] inline bool x509_time_unix(const mbedtls_x509_time& t, std::int64_t& out) {
    if (t.mon < 1 || t.mon > 12 || t.day < 1 || t.day > 31 || t.hour < 0 || t.hour > 23 || t.min < 0 ||
        t.min > 59 || t.sec < 0 || t.sec > 60)
        return false;
    const std::int64_t y = static_cast<std::int64_t>(t.year) - (t.mon <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t m = t.mon;
    const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + t.day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = era * 146097 + doe - 719468;
    out = days * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
    return true;
}

[[nodiscard]] inline std::int64_t real_steady_ns(void*) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace detail

// ---------------------------------------------------------------------------------------------------
// The anchor's EnrolCrypto.
// ---------------------------------------------------------------------------------------------------
class MbedtlsEnrolCrypto final : public enrol::EnrolCrypto {
public:
    using ClockFn = std::int64_t (*)(void* ctx) noexcept;  // mirrors SecureTransport::ClockFn
    static constexpr std::uint64_t kDefaultSkewS = 300;

    // The TrustStore is SHARED, not borrowed: Enrollment holds this object by shared_ptr, and a completion
    // arriving on another thread may still be checking a certificate after Enrollment (and the transport)
    // are gone. `skew_s` bounds how far in the future an issued certificate's notBefore may lie (the
    // custodian's clock may run ahead; peers, which allow no skew, accept the certificate once notBefore
    // passes). Pass the clock the TrustStore's rotations are driven by; the default is the real steady
    // clock, SecureTransport's default.
    explicit MbedtlsEnrolCrypto(std::shared_ptr<const TrustStore> trust, std::uint64_t skew_s = kDefaultSkewS,
                                ClockFn clock = &detail::real_steady_ns, void* clock_ctx = nullptr) noexcept
        : trust_(std::move(trust)), skew_s_(skew_s), clock_(clock), clock_ctx_(clock_ctx) {}

    [[nodiscard]] bool hmac_equal(std::span<const std::byte> secret, std::span<const std::byte> message,
                                  std::span<const std::byte> tag) const override {
        if (tag.size() != enrol::MacTag{}.size()) return false;
        std::array<unsigned char, 32> mac{};
        const bool ok = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), detail::uchars(secret),
                                        secret.size(), detail::uchars(message), message.size(), mac.data()) == 0 &&
                        mbedtls_ct_memcmp(mac.data(), detail::uchars(tag), mac.size()) == 0;
        quark::detail::secure_zero(mac.data(), mac.size());
        return ok;
    }

    // mbedTLS parses a CSR without checking its signature, so the proof of possession is verified here:
    // ecdsa-with-SHA256 over the CertificationRequestInfo, by the CSR's own P-256 key.
    [[nodiscard]] bool csr_public_key(std::span<const std::byte> csr_der,
                                      std::vector<std::byte>& spki_der) const override {
        if (csr_der.empty() || csr_der.size() > detail::kMaxCsrBytes) return false;
        detail::Csr csr;
        if (mbedtls_x509_csr_parse_der(&csr.v, detail::uchars(csr_der), csr_der.size()) != 0) return false;
        if (csr.v.raw.len != csr_der.size()) return false;  // no trailing bytes (mbedTLS 3.6 refuses them too)
        if (!detail::is_p256(csr.v.pk) || MBEDTLS_OID_CMP(MBEDTLS_OID_ECDSA_SHA256, &csr.v.sig_oid) != 0)
            return false;
        // CertificationRequest ::= SEQUENCE { certificationRequestInfo, signatureAlgorithm, signature BIT STRING }
        unsigned char* p = csr.v.cri.p + csr.v.cri.len;
        unsigned char* const end = csr.v.raw.p + csr.v.raw.len;
        std::size_t len = 0;
        if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return false;
        p += len;
        if (mbedtls_asn1_get_bitstring_null(&p, end, &len) != 0 || len == 0 || p + len != end) return false;
        std::array<unsigned char, 32> hash{};
        if (mbedtls_sha256(csr.v.cri.p, csr.v.cri.len, hash.data(), 0) != 0) return false;
        if (mbedtls_pk_verify(&csr.v.pk, MBEDTLS_MD_SHA256, hash.data(), hash.size(), p, len) != 0) return false;
        detail::Spki spki;
        if (!detail::spki_of(csr.v.pk, spki)) return false;
        const auto b = spki.bytes();
        spki_der.assign(reinterpret_cast<const std::byte*>(b.data()), reinterpret_cast<const std::byte*>(b.data()) + b.size());
        return true;
    }

    [[nodiscard]] bool issued_ok(const enrol::IssuedExpectation& expect,
                                 std::span<const std::byte> chain_der) const override {
        if (chain_der.empty() || chain_der.size() > detail::kMaxIssuedChainBytes) return false;
        detail::Crt chain;
        if (!detail::parse_der_concat(chain.v, chain_der)) return false;
        const mbedtls_x509_crt& leaf = chain.v;

        std::string_view cn;
        if (!detail::single_cn(leaf, cn) || cn != expect.cn) return false;
        detail::Spki spki;
        if (!detail::spki_of(leaf.pk, spki) ||
            !detail::same_bytes(spki.bytes(), {detail::uchars(expect.spki_der), expect.spki_der.size()}))
            return false;
        if (detail::ca_capable(leaf)) return false;

        constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2);
        if (expect.now_unix_s > kMax || skew_s_ > kMax || expect.max_validity_s > kMax) return false;
        const auto now = static_cast<std::int64_t>(expect.now_unix_s);
        std::int64_t not_before = 0, not_after = 0;
        if (!detail::x509_time_unix(leaf.valid_from, not_before) || !detail::x509_time_unix(leaf.valid_to, not_after))
            return false;
        if (not_before > not_after || not_before > now + static_cast<std::int64_t>(skew_s_) || not_after < now ||
            not_after > now + static_cast<std::int64_t>(expect.max_validity_s))
            return false;

        // The chain must end at a CONFIGURED root. `chain.v` holds the leaf plus any intermediates the
        // custodian sent; mbedtls_x509_crt_verify only ever terminates at `roots`, so a self-signed root
        // carried inside chain_der is not trusted. The intermediates' and roots' own validity is checked by
        // mbedTLS against the real wall clock; only the leaf's is judged on the anchor's clock (above).
        detail::Crt roots;
        const TrustStore::Snapshot snap = trust_->snapshot(clock_(clock_ctx_));
        bool any_root = false;
        for (const TrustedRoots* bundle : {snap.current.get(), snap.previous.get()}) {
            if (bundle == nullptr || bundle->ca_chain_der.empty()) continue;
            if (!detail::parse_der_concat(roots.v, bundle->ca_chain_der)) return false;
            any_root = true;
        }
        if (!any_root) return false;
        std::uint32_t flags = 0;
        return mbedtls_x509_crt_verify(&chain.v, &roots.v, nullptr, nullptr, &flags, &leaf_time_checked, nullptr) == 0 &&
               flags == 0;
    }

private:
    // The leaf's validity was checked above against the anchor's clock (with the notBefore skew); mbedTLS
    // would re-check it against the real clock with no skew. Its other flags, and every flag of the
    // intermediates and the root, stand.
    static int leaf_time_checked(void*, mbedtls_x509_crt*, int depth, std::uint32_t* flags) {
        if (depth == 0) *flags &= ~static_cast<std::uint32_t>(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED);
        return 0;
    }

    std::shared_ptr<const TrustStore> trust_;
    std::uint64_t skew_s_;
    ClockFn clock_;
    void* clock_ctx_;
};

// ---------------------------------------------------------------------------------------------------
// ADR-067 C5 at startup: refuse a node identity that is CA key material — its private key or certificate
// key equal to the key of any root in `trust` (current and still-in-window previous), or a CA-capable
// certificate (an issuing intermediate's). MbedtlsHandshakeEngine applies the same rule to every handshake.
// ---------------------------------------------------------------------------------------------------
[[nodiscard]] inline result<void> validate_identity_not_root(const TlsIdentity& own, const TrustStore::Snapshot& trust) {
    detail::Pk key;
    detail::Crt cert, roots;
    if (!detail::parse_private_key(key.v, own.private_key_der))
        return fail(errc::validation, "own identity private key does not parse (ADR-067 C5)");
    if (mbedtls_x509_crt_parse_der(&cert.v, detail::uchars(own.cert_chain_der), own.cert_chain_der.size()) != 0)
        return fail(errc::validation, "own identity certificate does not parse (ADR-067 C5)");
    for (const TrustedRoots* bundle : {trust.current.get(), trust.previous.get()}) {
        if (bundle == nullptr || bundle->ca_chain_der.empty()) continue;
        if (!detail::parse_der_concat(roots.v, bundle->ca_chain_der))
            return fail(errc::validation, "trust roots do not parse (ADR-067 C5)");
    }
    if (roots.v.raw.len == 0) return fail(errc::validation, "no trust roots configured (ADR-067 C5)");
    if (detail::key_is_a_root(key.v, roots.v) || detail::key_is_a_root(cert.v.pk, roots.v) || detail::ca_capable(cert.v))
        return fail(errc::validation, "own identity is CA key material: a CA key must never be in a Quark process "
                                      "(ADR-067 C5)");
    return {};
}

// ---------------------------------------------------------------------------------------------------
// The joiner's half (ADR-067 D4). The joiner keeps its key; only the CSR and the MAC are sent.
// ---------------------------------------------------------------------------------------------------

// A fresh ECDSA P-256 identity key, DER as mbedtls_pk_write_key_der writes it (SEC1 ECPrivateKey; what
// TlsIdentity::private_key_der holds for this adapter).
[[nodiscard]] inline result<std::vector<std::byte>> generate_identity_key() {
    detail::Pk pk;
    std::array<unsigned char, detail::kMaxKeyDerBytes> buf{};
    int len = -1;
    if (mbedtls_pk_setup(&pk.v, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk.v), &detail::SharedRng::random,
                            &detail::SharedRng::instance()) == 0)
        len = mbedtls_pk_write_key_der(&pk.v, buf.data(), buf.size());
    std::vector<std::byte> key_der;
    if (len > 0) {
        const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - static_cast<std::size_t>(len));
        key_der.assign(tail, tail + len);
    }
    quark::detail::secure_zero(buf.data(), buf.size());
    if (len <= 0) return fail(errc::unavailable, "P-256 key generation failed");
    return key_der;
}

// A PKCS#10 CSR for `key_der`, signed by it (the proof of possession). The anchor ignores every field
// but the public key, so the subject is a fixed placeholder.
[[nodiscard]] inline result<std::vector<std::byte>> make_enrol_csr(std::span<const std::byte> key_der) {
    detail::Pk pk;
    if (!detail::parse_private_key(pk.v, key_der)) return fail(errc::validation, "identity key does not parse");
    if (!detail::is_p256(pk.v)) return fail(errc::validation, "identity key is not ECDSA P-256 (ADR-067 D1)");
    mbedtls_x509write_csr w;
    mbedtls_x509write_csr_init(&w);
    mbedtls_x509write_csr_set_key(&w, &pk.v);
    mbedtls_x509write_csr_set_md_alg(&w, MBEDTLS_MD_SHA256);
    std::array<unsigned char, detail::kMaxCsrBytes> buf{};
    int len = -1;
    if (mbedtls_x509write_csr_set_subject_name(&w, "CN=quark-enrol") == 0)
        len = mbedtls_x509write_csr_der(&w, buf.data(), buf.size(), &detail::SharedRng::random,
                                        &detail::SharedRng::instance());
    mbedtls_x509write_csr_free(&w);
    if (len <= 0) return fail(errc::unavailable, "CSR encoding failed");
    const auto* tail = reinterpret_cast<const std::byte*>(buf.data() + buf.size() - static_cast<std::size_t>(len));
    return std::vector<std::byte>(tail, tail + len);
}

// HMAC-SHA256(ticket_secret, enrol_mac_message(cluster, ticket_id, csr)) — what the joiner sends with the CSR.
[[nodiscard]] inline result<enrol::MacTag> compute_enrol_mac(const enrol::TicketSecret& secret, ClusterId cluster,
                                                             const enrol::TicketId& ticket_id,
                                                             std::span<const std::byte> csr_der) {
    const std::vector<std::byte> msg = enrol::enrol_mac_message(cluster, ticket_id, csr_der);
    enrol::MacTag tag{};
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), detail::uchars(secret), secret.size(),
                        detail::uchars(msg), msg.size(), reinterpret_cast<unsigned char*>(tag.data())) != 0)
        return fail(errc::unavailable, "HMAC-SHA256 failed");
    return tag;
}

}  // namespace quark::adapters
